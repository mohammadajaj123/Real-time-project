#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include "member.h"

typedef void (*sighandler_t)(int);
sighandler_t sigset(int sig, sighandler_t disp);

/* Sleep for a fatigue-scaled duration: base grows linearly with moves,
 * is capped at max_ms, then a uniformly random jitter is added. */
static void rest_with_fatigue(int min_ms, int max_ms, int inc_ms, int moves) {
    int base = min_ms + moves * inc_ms;
    if (base > max_ms) base = max_ms;
    int headroom = max_ms - base;
    int jitter   = (headroom > 0) ? (rand() % (headroom + 1)) : 0;
    usleep((useconds_t)(base + jitter) * 1000);
}

/* Write a full PipeMsg to fd, looping until all bytes are sent. */
static int send_message(int fd, const PipeMsg *msg) {
    size_t done = 0;
    const char *p = (const char *)msg;
    while (done < sizeof(*msg)) {
        ssize_t n = write(fd, p + done, sizeof(*msg) - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* Read a full PipeMsg from fd, looping until all bytes are received. */
static int receive_message(int fd, PipeMsg *msg) {
    size_t done = 0;
    char *p = (char *)msg;
    while (done < sizeof(*msg)) {
        ssize_t n = read(fd, p + done, sizeof(*msg) - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* Open a FIFO with the given flags, retrying until the call succeeds. */
static int open_fifo_blocking(const char *path, int flags) {
    int fd;
    do { fd = open(path, flags); } while (fd < 0);
    return fd;
}

/* Two-step semaphore barrier: announce arrival, then wait to be released. */
static void wait_at_starting_line(int sem_id) {
    struct sembuf op;

    op.sem_num = SEM_ARRIVE;
    op.sem_op  = +1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);

    op.sem_num = SEM_DEPART;
    op.sem_op  = -1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

/* Picker (member 0): grab pieces from the pile, send forward, await verdict. */
void run_picker(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_out, const char *result_in_path,
                SharedState *shared, int sem_id)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 8));
    sigset(SIGPIPE, SIG_IGN);

    int result_in = open_fifo_blocking(result_in_path, O_RDONLY);

    wait_at_starting_line(sem_id);

    bool available[MAX_PIECES], rejected[MAX_PIECES];
    for (int i = 0; i < n_pieces; i++) { available[i] = true; rejected[i] = false; }

    int placed = 0, moves = 0;

    while (placed < n_pieces) {
        int pickable[MAX_PIECES], n_pickable = 0;
        for (int i = 0; i < n_pieces; i++)
            if (available[i] && !rejected[i]) pickable[n_pickable++] = i;

        if (n_pickable == 0) {
            for (int i = 0; i < n_pieces; i++) rejected[i] = false;
            for (int i = 0; i < n_pieces; i++)
                if (available[i]) pickable[n_pickable++] = i;
        }

        int chosen_idx = pickable[rand() % n_pickable];
        int serial     = shared->raw_serials[chosen_idx];

        shared->transit_serial[team] = serial;
        shared->transit_member[team] = 0;
        shared->transit_dir[team]    = 1;

        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        PipeMsg outgoing = { serial, 0 };
        if (send_message(forward_out, &outgoing) < 0) break;

        PipeMsg verdict;
        if (receive_message(result_in, &verdict) < 0) break;

        if (verdict.accepted) {
            available[chosen_idx] = false;
            memset(rejected, 0, n_pieces * sizeof(bool));
            placed++;
            shared->pieces_placed[team] = placed;
        } else {
            rejected[chosen_idx] = true;
        }
    }

    shared->transit_serial[team] = -1;
    close(result_in);
    close(forward_out);
}

/* Carrier (members 1 to N-2): relay pieces forward, then verdicts back, with rest in between. */
void run_carrier(int team, int position,
                 int min_ms, int max_ms, int inc_ms,
                 int forward_in, int forward_out,
                 const char *result_in_path,
                 const char *result_out_path,
                 SharedState *shared, int sem_id)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 4) ^ (unsigned)position);
    sigset(SIGPIPE, SIG_IGN);

    int result_out = open_fifo_blocking(result_out_path, O_WRONLY);
    int result_in  = open_fifo_blocking(result_in_path,  O_RDONLY);

    wait_at_starting_line(sem_id);

    int moves = 0;

    for (;;) {
        PipeMsg piece;
        if (receive_message(forward_in, &piece) < 0) break;

        shared->transit_member[team] = position;
        shared->transit_dir[team]    = 1;
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        if (send_message(forward_out, &piece) < 0) break;

        PipeMsg verdict;
        if (receive_message(result_in, &verdict) < 0) break;

        shared->transit_member[team] = position;
        shared->transit_dir[team]    = -1;
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        if (send_message(result_out, &verdict) < 0) break;
    }

    close(forward_in);
    close(forward_out);
    close(result_in);
    close(result_out);
}

/* Placer (member N-1): accept pieces in serial order, signal parent on completion. */
void run_placer(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_in, const char *result_out_path,
                SharedState *shared, int sem_id, pid_t parent_pid)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 2));
    sigset(SIGPIPE, SIG_IGN);

    int result_out = open_fifo_blocking(result_out_path, O_WRONLY);

    wait_at_starting_line(sem_id);

    int expected_idx = 0;
    int moves        = 0;

    while (expected_idx < n_pieces) {
        PipeMsg piece;
        if (receive_message(forward_in, &piece) < 0) break;

        shared->transit_member[team] = shared->n_members - 1;
        shared->transit_dir[team]    = -1;
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        PipeMsg verdict = { piece.serial, 0 };
        if (piece.serial == shared->sorted_serials[expected_idx]) {
            verdict.accepted = 1;
            expected_idx++;
        }

        if (send_message(result_out, &verdict) < 0) break;
    }

    shared->transit_serial[team] = -1;

    kill(parent_pid, (team == 0) ? SIGUSR1 : SIGUSR2);

    close(forward_in);
    close(result_out);
}
