#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <stdbool.h>

#define MAX_MEMBERS  32
#define MAX_PIECES   2000
#define MAX_TEAMS    2

enum { SEM_ARRIVE = 0, SEM_DEPART = 1 };

union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

typedef struct {
    int serial;
    int accepted;
} PipeMsg;

typedef struct {
    int n_members;
    int n_pieces;
    int n_wins_needed;

    int current_round;
    int team_wins[MAX_TEAMS];
    int pieces_placed[MAX_TEAMS];
    int game_over;
    int winner_team;

    int transit_serial[MAX_TEAMS];
    int transit_member[MAX_TEAMS];
    int transit_dir[MAX_TEAMS];

    int raw_serials[MAX_PIECES];
    int sorted_serials[MAX_PIECES];
} SharedState;

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

#endif
