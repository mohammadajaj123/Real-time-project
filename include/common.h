#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <stdbool.h>

#define MAX_MEMBERS  32
#define MAX_PIECES   2000
#define MAX_TEAMS    2

/* Message passed through anonymous pipes (forward) and named FIFOs (backward) */
typedef struct {
    int serial;    /* furniture piece serial number */
    int accepted;  /* 1 = sink accepted it, 0 = rejected; meaningful on backward pass */
} PipeMsg;

/* Shared memory between all processes (mapped before any fork) */
typedef struct {
    /* snapshot of config for display process */
    int n_members;
    int n_pieces;
    int n_wins_needed;

    /* round state (written by simulation, read by display) */
    int current_round;
    int team_wins[MAX_TEAMS];
    int pieces_placed[MAX_TEAMS];  /* placed in house this round */
    int game_over;
    int winner_team;               /* -1 = none yet, 0 or 1 */

    /* visualization: which piece is in transit and where */
    int transit_serial[MAX_TEAMS]; /* -1 = nothing in transit */
    int transit_member[MAX_TEAMS]; /* member index holding it */
    int transit_dir[MAX_TEAMS];    /* 1 = going forward, -1 = going backward */

    /* serial numbers for current round */
    int raw_serials[MAX_PIECES];    /* as handed to source (unsorted) */
    int sorted_serials[MAX_PIECES]; /* sorted ascending = delivery order for sink */

    /* start barrier: parent sets this to 1 after all members are forked,
     * so both teams begin at exactly the same time regardless of fork order */
    volatile int go;
} SharedState;

/* Configuration loaded from file */
typedef struct {
    int n_members;
    int n_pieces;
    int n_wins_needed;
    int min_pause_ms;
    int max_pause_ms;
    int pause_increment_ms;
    int provided_serials[MAX_PIECES];
    int n_provided;
} Config;

#endif /* COMMON_H */
