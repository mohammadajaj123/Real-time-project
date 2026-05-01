#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <omp.h>

#include "common.h"
#include "config.h"
#include "member.h"
#include "graphics.h"

/* ------------------------------------------------------------------ globals */

static SharedState             *g_state;
static volatile sig_atomic_t   g_round_over   = 0;
static volatile sig_atomic_t   g_round_winner = -1;
static pid_t                   g_sink_pids[2];

/* ------------------------------------------------------------------ signals */

static void sigusr1_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    if (!g_round_over) {
        pid_t from = info->si_pid;
        if      (from == g_sink_pids[0]) { g_round_winner = 0; g_round_over = 1; }
        else if (from == g_sink_pids[1]) { g_round_winner = 1; g_round_over = 1; }
    }
}

static void sigterm_handler(int sig) {
    (void)sig;
    /* clean exit for display process when parent is done */
    _exit(0);
}

/* ------------------------------------------------------------------ utility */

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/*
 * Generate n_pieces unique serial numbers.
 * OpenMP is used to initialise the candidate pool in parallel.
 */
static void generate_serials(const Config *cfg, SharedState *state) {
    int n = cfg->n_pieces;

    if (cfg->n_provided >= n) {
        memcpy(state->raw_serials, cfg->provided_serials, (size_t)n * sizeof(int));
    } else {
        /* build a pool of size n*5 and shuffle, then take the first n */
        int pool_size = n * 5;
        int pool[MAX_PIECES * 5];

        /* OpenMP: initialise pool in parallel */
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < pool_size; i++)
            pool[i] = i + 1;

        /* Fisher-Yates shuffle (sequential — depends on previous iteration) */
        for (int i = pool_size - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
        }

        memcpy(state->raw_serials, pool, (size_t)n * sizeof(int));
    }

    memcpy(state->sorted_serials, state->raw_serials, (size_t)n * sizeof(int));
    qsort(state->sorted_serials, (size_t)n, sizeof(int), cmp_int);
}

/* Create the N-1 named FIFOs for a team's backward channel */
static void setup_fifos(int team, int nm, char bwd[][64]) {
    for (int i = 0; i < nm - 1; i++) {
        snprintf(bwd[i], 64, "/tmp/rt_bwd_t%d_%d_%d", team, i, getpid());
        unlink(bwd[i]);
        if (mkfifo(bwd[i], 0666) < 0) {
            perror("mkfifo");
            exit(1);
        }
    }
}

static void remove_fifos(int team, int nm) {
    char path[64];
    for (int i = 0; i < nm - 1; i++) {
        snprintf(path, sizeof(path), "/tmp/rt_bwd_t%d_%d_%d", team, i, getpid());
        unlink(path);
    }
}

/*
 * Called inside each forked child.
 * Closes all pipe FDs the child does not own, then dispatches to the
 * appropriate run_* function based on member position.
 *
 * Forward pipes  fwd[i] : member i writes → member i+1 reads
 * Backward FIFOs bwd[i] : member i reads  ← member i+1 writes
 */
static void run_member_proc(int team, int mid, int nm, int n_pieces,
                             int fwd[][2], char bwd[][64],
                             const Config *cfg, SharedState *state,
                             pid_t parent_pid)
{
    /* FDs this member actually uses */
    int my_frd = (mid > 0)      ? fwd[mid - 1][0] : -1;
    int my_fwr = (mid < nm - 1) ? fwd[mid][1]      : -1;

    /* close all other pipe ends to allow correct EOF propagation */
    for (int i = 0; i < nm - 1; i++) {
        if (fwd[i][0] != my_frd) close(fwd[i][0]);
        if (fwd[i][1] != my_fwr) close(fwd[i][1]);
    }

    if (mid == 0) {
        /* source: reads bwd[0] FIFO, writes fwd[0] pipe */
        run_source(team, n_pieces,
                   cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                   my_fwr, bwd[0], state);

    } else if (mid == nm - 1) {
        /* sink: reads fwd[nm-2] pipe, writes bwd[nm-2] FIFO */
        run_sink(team, n_pieces,
                 cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                 my_frd, bwd[nm - 2], state, parent_pid);

    } else {
        /* intermediate: relay forward then backward */
        /* bwd_rd = bwd[mid]   (receives from member mid+1) */
        /* bwd_wr = bwd[mid-1] (sends to member mid-1)      */
        run_intermediate(team, mid,
                         cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                         my_frd, my_fwr,
                         bwd[mid], bwd[mid - 1], state);
    }
}

/* -------------------------------------------------------------------- main */

int main(int argc, char *argv[]) {
    const char *cfg_file = (argc > 1) ? argv[1] : "config.txt";
    Config cfg;

    if (config_load(cfg_file, &cfg) < 0)
        fprintf(stderr, "Config file '%s' not found — using defaults.\n", cfg_file);

    config_print(&cfg);

    /* ---- shared memory (accessible across all forks) ------------------ */
    g_state = mmap(NULL, sizeof(SharedState),
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_state == MAP_FAILED) { perror("mmap"); exit(1); }

    memset(g_state, 0, sizeof(*g_state));
    g_state->n_members     = cfg.n_members;
    g_state->n_pieces      = cfg.n_pieces;
    g_state->n_wins_needed = cfg.n_wins_needed;
    g_state->winner_team   = -1;
    g_state->transit_serial[0] = g_state->transit_serial[1] = -1;

    /* ---- signal handlers ----------------------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigusr1_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    srand((unsigned)time(NULL));

    /* flush before fork so buffered output isn't duplicated in the child */
    fflush(stdout);
    fflush(stderr);

    /* ---- fork display process ------------------------------------------ */
    pid_t disp_pid = fork();
    if (disp_pid < 0) { perror("fork display"); exit(1); }
    if (disp_pid == 0) {
        /* discard any parent stdio buffers inherited by the child */
        fclose(stdin);
        signal(SIGTERM, sigterm_handler);
        graphics_run(g_state);
        _exit(0);
    }

    /* ---- round loop ---------------------------------------------------- */
    int nm = cfg.n_members;

    for (int round = 1; ; round++) {
        printf("\n=== Round %d starting ===\n", round);
        fflush(stdout);

        /* prepare per-round state */
        generate_serials(&cfg, g_state);
        g_state->current_round     = round;
        g_state->pieces_placed[0]  = 0;
        g_state->pieces_placed[1]  = 0;
        g_state->transit_serial[0] = g_state->transit_serial[1] = -1;
        g_round_over               = 0;
        g_round_winner             = -1;

        /* create anonymous pipes (forward direction) */
        int fwd0[MAX_MEMBERS][2], fwd1[MAX_MEMBERS][2];
        for (int i = 0; i < nm - 1; i++) {
            if (pipe(fwd0[i]) < 0 || pipe(fwd1[i]) < 0) {
                perror("pipe"); exit(1);
            }
        }

        /* create named FIFOs (backward direction) */
        char bwd0[MAX_MEMBERS][64], bwd1[MAX_MEMBERS][64];
        setup_fifos(0, nm, bwd0);
        setup_fifos(1, nm, bwd1);

        /* fork all member processes */
        pid_t pids[2 * MAX_MEMBERS];
        int   np = 0;
        pid_t parent_pid = getpid();

        for (int i = 0; i < nm; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork team0"); exit(1); }
            if (pid == 0) {
                run_member_proc(0, i, nm, cfg.n_pieces, fwd0, bwd0,
                                &cfg, g_state, parent_pid);
                _exit(0);
            }
            if (i == nm - 1) g_sink_pids[0] = pid;
            pids[np++] = pid;
        }

        for (int i = 0; i < nm; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork team1"); exit(1); }
            if (pid == 0) {
                run_member_proc(1, i, nm, cfg.n_pieces, fwd1, bwd1,
                                &cfg, g_state, parent_pid);
                _exit(0);
            }
            if (i == nm - 1) g_sink_pids[1] = pid;
            pids[np++] = pid;
        }

        /* parent closes all pipe FDs — children own them now */
        for (int i = 0; i < nm - 1; i++) {
            close(fwd0[i][0]); close(fwd0[i][1]);
            close(fwd1[i][0]); close(fwd1[i][1]);
        }

        /* wait for SIGUSR1 from whichever sink finishes first */
        while (!g_round_over) pause();

        int winner = (int)g_round_winner;

        /* give the display a brief moment to show the last move */
        usleep(300000);

        /* terminate all member processes */
        for (int i = 0; i < np; i++) kill(pids[i], SIGTERM);
        for (int i = 0; i < np; i++) waitpid(pids[i], NULL, 0);

        /* clean up FIFOs */
        remove_fifos(0, nm);
        remove_fifos(1, nm);

        /* record result */
        g_state->team_wins[winner]++;
        printf("Team %d wins round %d!  Score: Team1=%d  Team2=%d\n",
               winner + 1, round,
               g_state->team_wins[0], g_state->team_wins[1]);
        fflush(stdout);

        if (g_state->team_wins[0] >= cfg.n_wins_needed ||
            g_state->team_wins[1] >= cfg.n_wins_needed) {
            break;
        }

        sleep(1); /* brief pause so display can show round result */
    }

    /* announce winner */
    int champ = (g_state->team_wins[0] >= cfg.n_wins_needed) ? 0 : 1;
    g_state->winner_team = champ;
    g_state->game_over   = 1;

    printf("\n*** Team %d wins the competition! ***\n", champ + 1);
    printf("Final score:  Team 1 = %d   Team 2 = %d\n\n",
           g_state->team_wins[0], g_state->team_wins[1]);
    fflush(stdout);

    sleep(4); /* let display process show the winner banner */

    kill(disp_pid, SIGTERM);
    waitpid(disp_pid, NULL, 0);

    munmap(g_state, sizeof(*g_state));
    return 0;
}
