#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include "member.h"

/* ------------------------------------------------------------------ helpers */

static void do_pause(int min_ms, int max_ms, int inc_ms, int moves) {
    /* base grows with the number of moves made (team members get tired) */
    int base = min_ms + moves * inc_ms;
    if (base > max_ms) base = max_ms;
    /* random variation in [0, remaining headroom] */
    int headroom = max_ms - base;
    int extra = (headroom > 0) ? (rand() % (headroom + 1)) : 0;
    int total = base + extra;
    usleep((useconds_t)total * 1000);
}

static int write_msg(int fd, const PipeMsg *m) {
    size_t done = 0;
    const char *p = (const char *)m;
    while (done < sizeof(*m)) {
        ssize_t n = write(fd, p + done, sizeof(*m) - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int read_msg(int fd, PipeMsg *m) {
    size_t done = 0;
    char *p = (char *)m;
    while (done < sizeof(*m)) {
        ssize_t n = read(fd, p + done, sizeof(*m) - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* Open a FIFO, retrying on EINTR.
 * O_WRONLY / O_RDONLY both block until the other end is opened.
 * Intermediate members open their write-end first to avoid chain deadlock. */
static int open_fifo(const char *path, int flags) {
    int fd;
    do { fd = open(path, flags); } while (fd < 0);
    return fd;
}

/* ------------------------------------------------------------------ source */

/*
 * Member 0 of each team.
 * Picks pieces randomly, sends them forward, waits for the backward result.
 *
 * Rejected pieces are put aside until a different piece is successfully
 * placed, then the rejected set is cleared and those pieces are retried.
 */
void run_source(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int fwd_wr, const char *bwd_rd_path,
                SharedState *state)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 8));
    signal(SIGPIPE, SIG_IGN);

    int bwd_rd = open_fifo(bwd_rd_path, O_RDONLY);

    bool available[MAX_PIECES], rejected[MAX_PIECES];
    for (int i = 0; i < n_pieces; i++) { available[i] = true; rejected[i] = false; }

    int placed = 0, moves = 0;

    while (placed < n_pieces) {
        /* build list of pickable piece indices */
        int pick[MAX_PIECES], np = 0;
        for (int i = 0; i < n_pieces; i++)
            if (available[i] && !rejected[i]) pick[np++] = i;

        /* safety: if all remaining are rejected, reset set and retry them */
        if (np == 0) {
            for (int i = 0; i < n_pieces; i++) rejected[i] = false;
            for (int i = 0; i < n_pieces; i++)
                if (available[i]) pick[np++] = i;
        }

        int idx    = pick[rand() % np];
        int serial = state->raw_serials[idx];

        /* update visualization */
        state->transit_serial[team] = serial;
        state->transit_member[team] = 0;
        state->transit_dir[team]    = 1;

        do_pause(min_ms, max_ms, inc_ms, moves++);

        PipeMsg msg = { serial, 0 };
        if (write_msg(fwd_wr, &msg) < 0) break;

        /* wait for the result to come back through the chain */
        PipeMsg result;
        if (read_msg(bwd_rd, &result) < 0) break;

        if (result.accepted) {
            available[idx] = false;
            memset(rejected, 0, n_pieces * sizeof(bool)); /* clear rejected set */
            placed++;
            state->pieces_placed[team] = placed;
        } else {
            rejected[idx] = true;
        }
    }

    state->transit_serial[team] = -1;
    close(bwd_rd);
    close(fwd_wr);
}

/* --------------------------------------------------------------- intermediate */

/*
 * Members 1 .. N-2.
 * Relay pieces forward; relay the result (accepted/rejected) backward.
 * Pause on each leg to simulate physical effort that grows with fatigue.
 *
 * Opens FIFO write-end before read-end to prevent the deadlock that would
 * occur if every intermediate tried to open its read-end first.
 */
void run_intermediate(int team, int member_id,
                      int min_ms, int max_ms, int inc_ms,
                      int fwd_rd, int fwd_wr,
                      const char *bwd_rd_path,  /* bwd[member_id]   */
                      const char *bwd_wr_path,  /* bwd[member_id-1] */
                      SharedState *state)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 4) ^ (unsigned)member_id);
    signal(SIGPIPE, SIG_IGN);

    /* write end first to break the potential open() chain deadlock */
    int bwd_wr_fd = open_fifo(bwd_wr_path, O_WRONLY);
    int bwd_rd_fd = open_fifo(bwd_rd_path, O_RDONLY);

    int moves = 0;

    for (;;) {
        PipeMsg msg;
        if (read_msg(fwd_rd, &msg) < 0) break;

        state->transit_member[team] = member_id;
        state->transit_dir[team]    = 1;
        do_pause(min_ms, max_ms, inc_ms, moves++);

        if (write_msg(fwd_wr, &msg) < 0) break;

        /* wait for result from the next member */
        PipeMsg result;
        if (read_msg(bwd_rd_fd, &result) < 0) break;

        state->transit_member[team] = member_id;
        state->transit_dir[team]    = -1;
        do_pause(min_ms, max_ms, inc_ms, moves++);

        if (write_msg(bwd_wr_fd, &result) < 0) break;
    }

    close(fwd_rd);
    close(fwd_wr);
    close(bwd_rd_fd);
    close(bwd_wr_fd);
}

/* -------------------------------------------------------------------- sink */

/*
 * Member N-1 of each team (the "house").
 * Receives pieces and checks whether the serial matches the next expected
 * piece in sorted order.  Accepted pieces stay; rejected ones are sent back.
 * When all pieces are accepted, signals the parent (SIGUSR1).
 */
void run_sink(int team, int n_pieces,
              int min_ms, int max_ms, int inc_ms,
              int fwd_rd, const char *bwd_wr_path,
              SharedState *state, pid_t parent_pid)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 2));
    signal(SIGPIPE, SIG_IGN);

    int bwd_wr = open_fifo(bwd_wr_path, O_WRONLY);

    int expected = 0; /* index into sorted_serials */
    int moves    = 0;

    while (expected < n_pieces) {
        PipeMsg msg;
        if (read_msg(fwd_rd, &msg) < 0) break;

        state->transit_member[team] = state->n_members - 1;
        state->transit_dir[team]    = -1;
        do_pause(min_ms, max_ms, inc_ms, moves++);

        PipeMsg result = { msg.serial, 0 };
        if (msg.serial == state->sorted_serials[expected]) {
            result.accepted = 1;
            expected++;
        }

        if (write_msg(bwd_wr, &result) < 0) break;
    }

    state->transit_serial[team] = -1;

    /* tell the parent coordinator this team has finished the round */
    kill(parent_pid, SIGUSR1);

    close(fwd_rd);
    close(bwd_wr);
}
