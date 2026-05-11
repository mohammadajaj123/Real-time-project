#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

/* Load configuration from a key=value text file. Returns 0 on success,
 * -1 if the file could not be opened (defaults are still populated). */
int  config_load(const char *path, Config *cfg);

/* Populate cfg with built-in default values. */
void config_defaults(Config *cfg);

/* Print the current configuration to stdout. */
void config_print(const Config *cfg);

#endif
