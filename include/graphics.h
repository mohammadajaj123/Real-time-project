#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "common.h"

/*
 * Runs the display loop in the calling process.
 * With USE_GRAPHICS defined: uses OpenGL/GLUT.
 * Otherwise: uses ANSI terminal output.
 * This function never returns (runs until killed).
 */
void graphics_run(SharedState *state);

#endif /* GRAPHICS_H */
