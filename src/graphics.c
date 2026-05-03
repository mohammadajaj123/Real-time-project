#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <GL/glut.h>
#include "graphics.h"

static SharedState *g_st;

/* ---- drawing helpers ------------------------------------------ */

static void draw_rect(float x, float y, float w, float h,
                       float r, float g, float b) {
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x,     y + h);
    glEnd();
}

static void draw_circle(float cx, float cy, float radius,
                         float r, float g, float b) {
    glColor3f(r, g, b);
    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);
        for (int i = 0; i <= 32; i++) {
            float a = i * 2.0f * 3.14159f / 32;
            glVertex2f(cx + radius * cosf(a), cy + radius * sinf(a));
        }
    glEnd();
}

static void draw_text(float x, float y, const char *str, float r, float g, float b) {
    glColor3f(r, g, b);
    glRasterPos2f(x, y);
    for (const char *c = str; *c; c++)
        glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *c);
}

/* ---- main display callback ------------------------------------ */

static void display_cb(void) {
    int W = glutGet(GLUT_WINDOW_WIDTH);
    int H = glutGet(GLUT_WINDOW_HEIGHT);

    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    SharedState *s = g_st;
    int nm = s->n_members;
    char buf[128];

    /* ---- title bar -------------------------------------------- */
    snprintf(buf, sizeof(buf), "Furniture Competition  Round: %d   "
             "Team 1 wins: %d   Team 2 wins: %d   (need %d)",
             s->current_round, s->team_wins[0], s->team_wins[1], s->n_wins_needed);
    draw_text(10, H - 20, buf, 1.0f, 1.0f, 0.3f);

    /* ---- two teams -------------------------------------------- */
    float team_colors[2][3] = { {0.2f,0.5f,1.0f}, {1.0f,0.35f,0.2f} };
    float row_y[2] = { (float)H * 0.65f, (float)H * 0.28f };
    float labels_y[2] = { row_y[0] + 55, row_y[1] + 55 };

    float margin  = 60.0f;
    float usable  = (float)W - 2 * margin;
    /* spacing between objects: pile | members | house */
    int   slots   = nm + 2;            /* pile + nm members + house */
    float step    = usable / (float)(slots - 1);

    for (int t = 0; t < 2; t++) {
        float *col = team_colors[t];
        float ry   = row_y[t];

        /* team label */
        snprintf(buf, sizeof(buf), "Team %d  [placed %d / %d]",
                 t + 1, s->pieces_placed[t], s->n_pieces);
        draw_text(margin, labels_y[t], buf, col[0], col[1], col[2]);

        /* furniture pile (source end) */
        int pile_remaining = s->n_pieces - s->pieces_placed[t];
        draw_rect(margin - 18, ry - 22, 36, 44, col[0]*0.5f, col[1]*0.5f, col[2]*0.5f);
        snprintf(buf, sizeof(buf), "%d", pile_remaining);
        draw_text(margin - 6, ry - 5, buf, 1, 1, 1);

        /* member circles */
        for (int m = 0; m < nm; m++) {
            float cx = margin + (float)(m + 1) * step;

            /* connector line */
            if (m < nm - 1) {
                float nx = margin + (float)(m + 2) * step;
                glColor3f(0.4f, 0.4f, 0.4f);
                glBegin(GL_LINES);
                    glVertex2f(cx + 18, ry);
                    glVertex2f(nx - 18, ry);
                glEnd();
            }

            draw_circle(cx, ry, 18, col[0]*0.7f, col[1]*0.7f, col[2]*0.7f);
            snprintf(buf, sizeof(buf), "M%d", m);
            draw_text(cx - 9, ry - 5, buf, 1, 1, 1);
        }

        /* piece in transit */
        if (s->transit_serial[t] >= 0) {
            int mem = s->transit_member[t];
            float px = margin + (float)(mem + 1) * step;
            draw_rect(px - 10, ry + 25, 20, 20,
                      col[0], col[1], col[2]);
            snprintf(buf, sizeof(buf), "%d", s->transit_serial[t]);
            draw_text(px - 8, ry + 30, buf, 0, 0, 0);

            /* direction arrow */
            float ax = px + (s->transit_dir[t] > 0 ? 14 : -14);
            glColor3f(1, 1, 0);
            glBegin(GL_TRIANGLES);
            if (s->transit_dir[t] > 0) {
                glVertex2f(ax,      ry + 35);
                glVertex2f(ax - 8,  ry + 30);
                glVertex2f(ax - 8,  ry + 40);
            } else {
                glVertex2f(ax,      ry + 35);
                glVertex2f(ax + 8,  ry + 30);
                glVertex2f(ax + 8,  ry + 40);
            }
            glEnd();
        }

        /* house (sink end) */
        float hx = margin + (float)(nm + 1) * step;
        draw_rect(hx - 22, ry - 28, 44, 56, 0.2f, 0.55f, 0.2f);
        snprintf(buf, sizeof(buf), "H%d", t + 1);
        draw_text(hx - 8, ry - 5, buf, 1, 1, 1);
        snprintf(buf, sizeof(buf), "%d", s->pieces_placed[t]);
        draw_text(hx - 6, ry - 20, buf, 0.9f, 0.9f, 0.3f);
    }

    /* ---- game over banner ------------------------------------- */
    if (s->game_over) {
        snprintf(buf, sizeof(buf), "*** Team %d wins the competition! ***",
                 s->winner_team + 1);
        draw_text((float)W / 2 - 140, (float)H / 2, buf, 0.0f, 1.0f, 0.3f);
    }

    glutSwapBuffers();
}

static void timer_cb(int val) {
    (void)val;
    glutPostRedisplay();
    glutTimerFunc(40, timer_cb, 0); /* ~25 fps */
}

void graphics_run(SharedState *state) {
    g_st = state;
    int argc = 0;
    glutInit(&argc, NULL);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(1100, 500);
    glutInitWindowPosition(100, 100);
    glutCreateWindow("Furniture Competition");
    glutDisplayFunc(display_cb);
    glutTimerFunc(40, timer_cb, 0);
    glutMainLoop();
}
