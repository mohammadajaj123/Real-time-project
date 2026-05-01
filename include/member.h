#ifndef MEMBER_H
#define MEMBER_H

#include "common.h"
#include <sys/types.h>

/*
 * Each run_* function is called in a child process after fork().
 * The child handles IPC setup (FIFO open) and then loops
 * until its pipe/FIFO is closed (SIGTERM from parent).
 *
 * Forward communication  : anonymous pipes  (source -> ... -> sink)
 * Backward communication : named FIFOs      (sink   -> ... -> source)
 *
 * FIFO naming: /tmp/rt_bwd_t{team}_{i}
 *   bwd[i] connects member i (reader) and member i+1 (writer).
 */

void run_source(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int fwd_wr, const char *bwd_rd_path,
                SharedState *state);

void run_intermediate(int team, int member_id,
                      int min_ms, int max_ms, int inc_ms,
                      int fwd_rd, int fwd_wr,
                      const char *bwd_rd_path,
                      const char *bwd_wr_path,
                      SharedState *state);

void run_sink(int team, int n_pieces,
              int min_ms, int max_ms, int inc_ms,
              int fwd_rd, const char *bwd_wr_path,
              SharedState *state, pid_t parent_pid);

#endif /* MEMBER_H */
