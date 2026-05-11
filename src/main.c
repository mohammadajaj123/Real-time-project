#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <time.h>
#include <omp.h>

#include "common.h"
#include "config.h"
#include "member.h"
#include "graphics.h"

typedef void (*sighandler_t)(int);
sighandler_t sigset(int sig, sighandler_t disp);

static SharedState              *g_shared;
static int                       g_shm_id;
static int                       g_sem_id;
static volatile sig_atomic_t     g_round_finished = 0;
static volatile sig_atomic_t     g_winning_team   = -1;

/* SIGUSR1 handler: record team 1 as the round winner. */
static void on_team1_finished(int sig) {
    (void)sig;
    if (!g_round_finished) { g_winning_team = 0; g_round_finished = 1; }
}

/* SIGUSR2 handler: record team 2 as the round winner. */
static void on_team2_finished(int sig) {
    (void)sig;
    if (!g_round_finished) { g_winning_team = 1; g_round_finished = 1; }
}

/* SIGTERM handler used by the display child to exit cleanly. */
static void on_terminate(int sig) {
    (void)sig;
    _exit(0);
}

/* qsort comparator for ints in ascending order. */
static int compare_ints(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* Build the round's piece list using Floyd's sampling algorithm,
 * then make a sorted copy that the placer uses as the expected order. */
static void prepare_round_pieces(const Config *cfg, SharedState *shared) {
    int n_pieces = cfg->n_pieces;

    if (cfg->n_provided >= n_pieces) {
        memcpy(shared->raw_serials, cfg->provided_serials,
               (size_t)n_pieces * sizeof(int));
    } else {
        int pool_size = n_pieces * 5;
        bool taken[MAX_PIECES * 5 + 1];

        #pragma omp parallel for schedule(static)
        for (int i = 0; i <= pool_size; i++) taken[i] = false;

        int out = 0;
        for (int j = pool_size - n_pieces + 1; j <= pool_size; j++) {
            int t = 1 + rand() % j;
            if (taken[t]) {
                shared->raw_serials[out++] = j;
                taken[j] = true;
            } else {
                shared->raw_serials[out++] = t;
                taken[t] = true;
            }
        }
    }

    memcpy(shared->sorted_serials, shared->raw_serials,
           (size_t)n_pieces * sizeof(int));
    qsort(shared->sorted_serials, (size_t)n_pieces, sizeof(int), compare_ints);
}

/* Create the team_size-1 backward-channel FIFOs for one team. */
static void create_result_fifos(int team, int team_size, char paths[][64]) {
    for (int i = 0; i < team_size - 1; i++) {
        snprintf(paths[i], 64, "/tmp/rt_bwd_t%d_%d_%d", team, i, getpid());
        unlink(paths[i]);
        if (mknod(paths[i], S_IFIFO | 0666, 0) < 0) {
            perror("mknod");
            exit(1);
        }
    }
}

/* Remove the backward-channel FIFOs for one team. */
static void delete_result_fifos(int team, int team_size) {
    char path[64];
    for (int i = 0; i < team_size - 1; i++) {
        snprintf(path, sizeof(path), "/tmp/rt_bwd_t%d_%d_%d", team, i, getpid());
        unlink(path);
    }
}

/* Detach and remove the System V shared memory and semaphore set. */
static void release_shared_resources(void) {
    if (g_shared) {
        shmdt(g_shared);
        g_shared = NULL;
    }
    if (g_shm_id >= 0) shmctl(g_shm_id, IPC_RMID, 0);
    if (g_sem_id >= 0) semctl(g_sem_id, 0, IPC_RMID, 0);
    g_shm_id = g_sem_id = -1;
}

/* SIGINT handler: release shared resources before exiting. */
static void on_interrupt(int sig) {
    (void)sig;
    release_shared_resources();
    _exit(1);
}

/* Inside each forked child: close pipe FDs not owned by this member,
 * then dispatch to the picker / carrier / placer role. */
static void dispatch_team_member(int team, int position, int team_size, int n_pieces,
                                 int forward_pipes[][2], char result_paths[][64],
                                 const Config *cfg, SharedState *shared, int sem_id,
                                 pid_t parent_pid)
{
    int forward_in  = (position > 0)              ? forward_pipes[position - 1][0] : -1;
    int forward_out = (position < team_size - 1)  ? forward_pipes[position][1]     : -1;

    for (int i = 0; i < team_size - 1; i++) {
        if (forward_pipes[i][0] != forward_in)  close(forward_pipes[i][0]);
        if (forward_pipes[i][1] != forward_out) close(forward_pipes[i][1]);
    }

    if (position == 0) {
        run_picker(team, n_pieces,
                   cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                   forward_out, result_paths[0], shared, sem_id);

    } else if (position == team_size - 1) {
        run_placer(team, n_pieces,
                   cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                   forward_in, result_paths[team_size - 2],
                   shared, sem_id, parent_pid);

    } else {
        run_carrier(team, position,
                    cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                    forward_in, forward_out,
                    result_paths[position], result_paths[position - 1],
                    shared, sem_id);
    }
}

/* Program entry: set up IPC, fork the display, run the round loop, announce winner. */
int main(int argc, char *argv[]) {
    const char *cfg_file = (argc > 1) ? argv[1] : "config.txt";
    Config cfg;

    if (config_load(cfg_file, &cfg) < 0)
        fprintf(stderr, "Config file '%s' not found - using defaults.\n", cfg_file);

    config_print(&cfg);

    g_shm_id = g_sem_id = -1;
    g_shared = NULL;

    g_shm_id = shmget(IPC_PRIVATE, sizeof(SharedState), IPC_CREAT | 0666);
    if (g_shm_id < 0) { perror("shmget"); exit(1); }

    g_shared = (SharedState *)shmat(g_shm_id, NULL, 0);
    if (g_shared == (void *)-1) { perror("shmat"); exit(1); }

    memset(g_shared, 0, sizeof(*g_shared));
    g_shared->n_members     = cfg.n_members;
    g_shared->n_pieces      = cfg.n_pieces;
    g_shared->n_wins_needed = cfg.n_wins_needed;
    g_shared->winner_team   = -1;
    g_shared->transit_serial[0] = g_shared->transit_serial[1] = -1;

    g_sem_id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0666);
    if (g_sem_id < 0) { perror("semget"); release_shared_resources(); exit(1); }

    sigset(SIGUSR1, on_team1_finished);
    sigset(SIGUSR2, on_team2_finished);
    sigset(SIGINT,  on_interrupt);

    srand((unsigned)time(NULL));

    fflush(stdout);
    fflush(stderr);

    pid_t display_pid = fork();
    if (display_pid < 0) { perror("fork display"); release_shared_resources(); exit(1); }
    if (display_pid == 0) {
        fclose(stdin);
        sigset(SIGTERM, on_terminate);
        graphics_run(g_shared);
        _exit(0);
    }

    int team_size = cfg.n_members;

    for (int round = 1; ; round++) {
        printf("\n=== Round %d starting ===\n", round);
        fflush(stdout);

        prepare_round_pieces(&cfg, g_shared);
        g_shared->current_round     = round;
        g_shared->pieces_placed[0]  = 0;
        g_shared->pieces_placed[1]  = 0;
        g_shared->transit_serial[0] = g_shared->transit_serial[1] = -1;
        g_round_finished            = 0;
        g_winning_team              = -1;

        union semun arg;
        arg.val = 0;
        semctl(g_sem_id, SEM_ARRIVE, SETVAL, arg);
        semctl(g_sem_id, SEM_DEPART, SETVAL, arg);

        int forward_team1[MAX_MEMBERS][2], forward_team2[MAX_MEMBERS][2];
        for (int i = 0; i < team_size - 1; i++) {
            if (pipe(forward_team1[i]) < 0 || pipe(forward_team2[i]) < 0) {
                perror("pipe"); release_shared_resources(); exit(1);
            }
        }

        char result_team1[MAX_MEMBERS][64], result_team2[MAX_MEMBERS][64];
        create_result_fifos(0, team_size, result_team1);
        create_result_fifos(1, team_size, result_team2);

        pid_t worker_pids[2 * MAX_MEMBERS];
        int   n_workers = 0;
        pid_t parent_pid = getpid();

        for (int i = 0; i < team_size; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork team1"); release_shared_resources(); exit(1); }
            if (pid == 0) {
                dispatch_team_member(0, i, team_size, cfg.n_pieces,
                                     forward_team1, result_team1,
                                     &cfg, g_shared, g_sem_id, parent_pid);
                _exit(0);
            }
            worker_pids[n_workers++] = pid;
        }

        for (int i = 0; i < team_size; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork team2"); release_shared_resources(); exit(1); }
            if (pid == 0) {
                dispatch_team_member(1, i, team_size, cfg.n_pieces,
                                     forward_team2, result_team2,
                                     &cfg, g_shared, g_sem_id, parent_pid);
                _exit(0);
            }
            worker_pids[n_workers++] = pid;
        }

        for (int i = 0; i < team_size - 1; i++) {
            close(forward_team1[i][0]); close(forward_team1[i][1]);
            close(forward_team2[i][0]); close(forward_team2[i][1]);
        }

        struct sembuf op;
        op.sem_num = SEM_ARRIVE;
        op.sem_op  = -(short)(2 * team_size);
        op.sem_flg = 0;
        if (semop(g_sem_id, &op, 1) < 0) {
            perror("semop arrive"); release_shared_resources(); exit(1);
        }

        op.sem_num = SEM_DEPART;
        op.sem_op  = +(short)(2 * team_size);
        op.sem_flg = 0;
        if (semop(g_sem_id, &op, 1) < 0) {
            perror("semop depart"); release_shared_resources(); exit(1);
        }

        while (!g_round_finished) pause();

        int winner = (int)g_winning_team;

        usleep(300000);

        for (int i = 0; i < n_workers; i++) kill(worker_pids[i], SIGTERM);
        for (int i = 0; i < n_workers; i++) waitpid(worker_pids[i], NULL, 0);

        delete_result_fifos(0, team_size);
        delete_result_fifos(1, team_size);

        g_shared->team_wins[winner]++;
        printf("Team %d wins round %d!  Score: Team1=%d  Team2=%d\n",
               winner + 1, round,
               g_shared->team_wins[0], g_shared->team_wins[1]);
        fflush(stdout);

        if (g_shared->team_wins[0] >= cfg.n_wins_needed ||
            g_shared->team_wins[1] >= cfg.n_wins_needed) {
            break;
        }

        sleep(1);
    }

    int champion = (g_shared->team_wins[0] >= cfg.n_wins_needed) ? 0 : 1;
    g_shared->winner_team = champion;
    g_shared->game_over   = 1;

    printf("\n*** Team %d wins the competition! ***\n", champion + 1);
    printf("Final score:  Team 1 = %d   Team 2 = %d\n\n",
           g_shared->team_wins[0], g_shared->team_wins[1]);
    fflush(stdout);

    sleep(4);

    kill(display_pid, SIGTERM);
    waitpid(display_pid, NULL, 0);

    release_shared_resources();
    return 0;
}
