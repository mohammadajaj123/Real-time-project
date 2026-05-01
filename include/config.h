#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

/* Load configuration from key=value text file.
 * Returns 0 on success, -1 if file could not be opened (defaults are kept). */
int  config_load(const char *path, Config *cfg);
void config_defaults(Config *cfg);
void config_print(const Config *cfg);

#endif /* CONFIG_H */
