#ifndef MEMBER_H
#define MEMBER_H

#include "common.h"
#include <sys/types.h>

/* Picker (member 0): grab pieces from the pile, send forward, await verdict. */
void run_picker(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_out, const char *result_in_path,
                SharedState *shared, int sem_id);

/* Carrier (members 1..N-2): relay pieces forward, relay verdicts back. */
void run_carrier(int team, int position,
                 int min_ms, int max_ms, int inc_ms,
                 int forward_in, int forward_out,
                 const char *result_in_path,
                 const char *result_out_path,
                 SharedState *shared, int sem_id);

/* Placer (member N-1): receive pieces, accept in serial order, signal parent on completion. */
void run_placer(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_in, const char *result_out_path,
                SharedState *shared, int sem_id, pid_t parent_pid);

#endif
