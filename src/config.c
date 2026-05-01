#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e - 1))) e--;
    *e = '\0';
    return s;
}

void config_defaults(Config *cfg) {
    cfg->n_members          = 4;
    cfg->n_pieces           = 20;
    cfg->n_wins_needed      = 3;
    cfg->min_pause_ms       = 10;
    cfg->max_pause_ms       = 200;
    cfg->pause_increment_ms = 3;
    cfg->n_provided         = 0;
}

int config_load(const char *path, Config *cfg) {
    config_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = trim(p);
        char *val = trim(eq + 1);

        if      (strcmp(key, "n_members")          == 0) cfg->n_members          = atoi(val);
        else if (strcmp(key, "n_pieces")            == 0) cfg->n_pieces           = atoi(val);
        else if (strcmp(key, "n_wins_needed")       == 0) cfg->n_wins_needed      = atoi(val);
        else if (strcmp(key, "min_pause_ms")        == 0) cfg->min_pause_ms       = atoi(val);
        else if (strcmp(key, "max_pause_ms")        == 0) cfg->max_pause_ms       = atoi(val);
        else if (strcmp(key, "pause_increment_ms")  == 0) cfg->pause_increment_ms = atoi(val);
        else if (strcmp(key, "serials") == 0) {
            char *tok = strtok(val, ",");
            while (tok && cfg->n_provided < MAX_PIECES) {
                cfg->provided_serials[cfg->n_provided++] = atoi(trim(tok));
                tok = strtok(NULL, ",");
            }
        }
    }
    fclose(f);

    /* clamp to sane ranges */
    if (cfg->n_members < 2)            cfg->n_members = 2;
    if (cfg->n_members > MAX_MEMBERS)  cfg->n_members = MAX_MEMBERS;
    if (cfg->n_pieces  < 1)            cfg->n_pieces  = 1;
    if (cfg->n_pieces  > MAX_PIECES)   cfg->n_pieces  = MAX_PIECES;

    if (cfg->n_provided > 0 && cfg->n_provided < cfg->n_pieces) {
        fprintf(stderr, "Warning: %d serials provided but %d pieces needed — "
                        "switching to random.\n", cfg->n_provided, cfg->n_pieces);
        cfg->n_provided = 0;
    }

    return 0;
}

void config_print(const Config *cfg) {
    printf("Configuration:\n");
    printf("  members/team     : %d\n", cfg->n_members);
    printf("  pieces/round     : %d\n", cfg->n_pieces);
    printf("  wins to win      : %d\n", cfg->n_wins_needed);
    printf("  pause range (ms) : %d - %d (+%d/move)\n",
           cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms);
    if (cfg->n_provided > 0)
        printf("  serial source    : user-provided (%d values)\n", cfg->n_provided);
    else
        printf("  serial source    : random\n");
    printf("\n");
}
