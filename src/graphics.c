/* ============================================================
 * graphics.c
 * ------------------------------------------------------------
 * Implementation of the visual display using OpenGL/GLUT.
 * Runs in its own forked child process and continuously reads
 * from SharedState to render both teams' state on screen.
 *
 * Display elements:
 *   - title bar: round number and per-team win count
 *   - per team: pile + chain of members (circles) + house
 *   - moving square representing the carried piece + arrow
 *   - banner when the competition ends
 *
 * تنفيذ واجهة العرض البصري باستخدام OpenGL/GLUT. تعمل في
 * عملية ابن مفروقة مستقلة وتقرأ باستمرار من بنية الحالة
 * المشتركة لرسم حالة الفريقين على الشاشة.
 *
 * عناصر العرض:
 *   - شريط العنوان: رقم الجولة وعدد فوز كل فريق
 *   - لكل فريق: كومة الأثاث + سلسلة الأعضاء (دوائر) + البيت
 *   - مربع متحرك يمثل القطعة المحمولة + سهم بالاتجاه
 *   - banner عند انتهاء المنافسة
 * ============================================================ */

#include <stdio.h>          /* snprintf | تنسيق النصوص */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>           /* cosf, sinf for circle | لرسم الدائرة */
#include <GL/glut.h>        /* OpenGL Utility Toolkit | مكتبة الرسم */
#include "graphics.h"

/* Pointer to shared memory; stored globally because GLUT
 * callbacks don't accept a user-data parameter.
 *
 * مؤشر إلى الذاكرة المشتركة. نحفظه عالمياً لأن callbacks الـ
 * GLUT لا تأخذ user-data parameter. */
static SharedState *g_st;

/* Draw a filled axis-aligned rectangle of size w x h at (x, y)
 * with color (r, g, b). Uses a QUADS primitive (4 vertices).
 *
 * يرسم مستطيلاً مملوءاً بإحداثيات (x, y) وحجم (w, h) ولون
 * (r, g, b). يستخدم primitive من نوع QUADS بأربعة رؤوس. */
static void draw_rect(float x, float y, float w, float h,
                       float r, float g, float b) {
    glColor3f(r, g, b);                     /* set current color | عيّن اللون الحالي */
    glBegin(GL_QUADS);                      /* start a quad | ابدأ رسم quad */
        glVertex2f(x,     y);               /* bottom-left  | السفلى اليسرى */
        glVertex2f(x + w, y);               /* bottom-right | السفلى اليمنى */
        glVertex2f(x + w, y + h);           /* top-right    | العليا اليمنى */
        glVertex2f(x,     y + h);           /* top-left     | العليا اليسرى */
    glEnd();
}

/* Draw a filled circle using a TRIANGLE_FAN. The center is the
 * first vertex; then 33 vertices on the perimeter (32 triangles).
 *
 * يرسم دائرة مملوءة باستخدام TRIANGLE_FAN. المركز هو الرأس
 * الأول، ثم 33 رأس على المحيط (32 مثلث). */
static void draw_circle(float cx, float cy, float radius,
                         float r, float g, float b) {
    glColor3f(r, g, b);
    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);                 /* center | مركز الدائرة */
        for (int i = 0; i <= 32; i++) {
            float a = i * 2.0f * 3.14159f / 32;     /* angle in radians | الزاوية بالراديان */
            glVertex2f(cx + radius * cosf(a), cy + radius * sinf(a));
        }
    glEnd();
}

/* Draw a string at (x, y) in color (r, g, b) using
 * glutBitmapCharacter for each character.
 *
 * يرسم نصاً عند الإحداثيات (x, y) باللون (r, g, b) باستخدام
 * glutBitmapCharacter لكل حرف على حدة. */
static void draw_text(float x, float y, const char *str, float r, float g, float b) {
    glColor3f(r, g, b);
    glRasterPos2f(x, y);                    /* set start position | عيّن موقع البدء */
    for (const char *c = str; *c; c++)
        glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *c);
}

/* GLUT display callback: invoked when a redraw is needed.
 * Reads everything from g_st (shared memory) and renders.
 *
 * الـ display callback الخاص بـ GLUT: يُستدعى عند الحاجة
 * لإعادة الرسم. يقرأ كل المعلومات من g_st (الذاكرة المشتركة)
 * ويرسم المشهد. */
static void display_cb(void) {
    /* Get current window size (may change on resize).
     * استرجع أبعاد النافذة الحالية (قد تتغير عند تغيير الحجم). */
    int W = glutGet(GLUT_WINDOW_WIDTH);
    int H = glutGet(GLUT_WINDOW_HEIGHT);

    /* === Set up 2D projection ===
     * === تهيئة الـ projection للرسم ثنائي الأبعاد === */
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);             /* pixel coords, (0,0) bottom-left | (0,0) أسفل اليسار */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Clear the screen with a dark grey color.
     * امسح الشاشة بلون رمادي داكن. */
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    SharedState *s = g_st;                  /* shorthand | اختصار */
    int nm = s->n_members;                  /* members per team | عدد الأعضاء في الفريق */
    char buf[128];                          /* temp buffer for snprintf | مخزن مؤقت */

    /* === Title bar at the top of the screen ===
     * === شريط العنوان أعلى الشاشة === */
    snprintf(buf, sizeof(buf), "Furniture Competition  Round: %d   "
             "Team 1 wins: %d   Team 2 wins: %d   (need %d)",
             s->current_round, s->team_wins[0], s->team_wins[1], s->n_wins_needed);
    draw_text(10, H - 20, buf, 1.0f, 1.0f, 0.3f);   /* yellow | لون أصفر */

    /* === Geometry settings for both teams ===
     * Team colors: blue for team 1, red for team 2.
     *
     * === الإعدادات الهندسية للفريقين ===
     * ألوان الفريقين: أزرق للفريق الأول، أحمر للفريق الثاني. */
    float team_colors[2][3] = { {0.2f,0.5f,1.0f}, {1.0f,0.35f,0.2f} };
    /* y position for each team (team 1 upper, team 2 lower)
     * قيمة y لكل فريق (الأول أعلى والثاني أسفل) */
    float row_y[2] = { (float)H * 0.65f, (float)H * 0.28f };
    /* y position for the label above each row
     * قيمة y للتسمية فوق كل صف */
    float labels_y[2] = { row_y[0] + 55, row_y[1] + 55 };

    /* Horizontal layout: pile - members - house, evenly spaced.
     * تخطيط أفقي: pile ثم الأعضاء ثم house موزعة بالتساوي. */
    float margin  = 60.0f;                  /* left/right margin | هامش يمين ويسار */
    float usable  = (float)W - 2 * margin;  /* usable width | العرض المتاح */
    int   slots   = nm + 2;                 /* pile + members + house | كومة + أعضاء + بيت */
    float step    = usable / (float)(slots - 1);    /* spacing between two slots | المسافة بين عنصرين */

    /* === Render each team ===
     * === رسم كل فريق === */
    for (int t = 0; t < 2; t++) {
        float *col = team_colors[t];
        float ry   = row_y[t];

        /* Team label and info | تسمية الفريق ومعلوماته */
        snprintf(buf, sizeof(buf), "Team %d  [placed %d / %d]",
                 t + 1, s->pieces_placed[t], s->n_pieces);
        draw_text(margin, labels_y[t], buf, col[0], col[1], col[2]);

        /* === Furniture pile (left side) === | === كومة الأثاث (يسار) === */
        int pile_remaining = s->n_pieces - s->pieces_placed[t];
        draw_rect(margin - 18, ry - 22, 36, 44, col[0]*0.5f, col[1]*0.5f, col[2]*0.5f);
        snprintf(buf, sizeof(buf), "%d", pile_remaining);
        draw_text(margin - 6, ry - 5, buf, 1, 1, 1);

        /* === Team members (circles) === | === أعضاء الفريق (دوائر) === */
        for (int m = 0; m < nm; m++) {
            float cx = margin + (float)(m + 1) * step;

            /* Connector line between adjacent members
             * خط رابط بين كل عضوين متجاورين */
            if (m < nm - 1) {
                float nx = margin + (float)(m + 2) * step;
                glColor3f(0.4f, 0.4f, 0.4f);
                glBegin(GL_LINES);
                    glVertex2f(cx + 18, ry);
                    glVertex2f(nx - 18, ry);
                glEnd();
            }

            /* The circle (member) | الدائرة (العضو) */
            draw_circle(cx, ry, 18, col[0]*0.7f, col[1]*0.7f, col[2]*0.7f);
            /* Member label "M0", "M1", ... | اسم العضو M0, M1, ... */
            snprintf(buf, sizeof(buf), "M%d", m);
            draw_text(cx - 9, ry - 5, buf, 1, 1, 1);
        }

        /* === Currently carried piece (if any) ===
         * === القطعة المحمولة حالياً (إن وجدت) === */
        if (s->transit_serial[t] >= 0) {
            int mem = s->transit_member[t];
            float px = margin + (float)(mem + 1) * step;

            /* Colored square holding the piece serial
             * مربع ملون يحمل رقم القطعة */
            draw_rect(px - 10, ry + 25, 20, 20, col[0], col[1], col[2]);
            snprintf(buf, sizeof(buf), "%d", s->transit_serial[t]);
            draw_text(px - 8, ry + 30, buf, 0, 0, 0);

            /* Direction arrow (yellow) | سهم يدل على الاتجاه (لون أصفر) */
            float ax = px + (s->transit_dir[t] > 0 ? 14 : -14);
            glColor3f(1, 1, 0);
            glBegin(GL_TRIANGLES);
            if (s->transit_dir[t] > 0) {
                /* Right-pointing arrow | سهم لليمين */
                glVertex2f(ax,      ry + 35);
                glVertex2f(ax - 8,  ry + 30);
                glVertex2f(ax - 8,  ry + 40);
            } else {
                /* Left-pointing arrow | سهم لليسار */
                glVertex2f(ax,      ry + 35);
                glVertex2f(ax + 8,  ry + 30);
                glVertex2f(ax + 8,  ry + 40);
            }
            glEnd();
        }

        /* === The house (right side) === | === البيت (يمين) === */
        float hx = margin + (float)(nm + 1) * step;
        draw_rect(hx - 22, ry - 28, 44, 56, 0.2f, 0.55f, 0.2f);     /* green | لون أخضر */
        snprintf(buf, sizeof(buf), "H%d", t + 1);
        draw_text(hx - 8, ry - 5, buf, 1, 1, 1);
        /* Counter for placed pieces | عداد القطع الموضوعة */
        snprintf(buf, sizeof(buf), "%d", s->pieces_placed[t]);
        draw_text(hx - 6, ry - 20, buf, 0.9f, 0.9f, 0.3f);
    }

    /* === Game-over banner === | === banner نهاية اللعبة === */
    if (s->game_over) {
        snprintf(buf, sizeof(buf), "*** Team %d wins the competition! ***",
                 s->winner_team + 1);
        draw_text((float)W / 2 - 140, (float)H / 2, buf, 0.0f, 1.0f, 0.3f);
    }

    /* Swap buffers (double buffering to avoid flicker).
     * بدّل الـ buffers (double buffering لمنع التذبذب). */
    glutSwapBuffers();
}

/* GLUT timer callback: called every 40ms (~25 fps) to request
 * a redraw. Reschedules itself via glutTimerFunc to keep going.
 *
 * الـ timer callback الخاص بـ GLUT: يُستدعى كل 40 ميلي ثانية
 * (~25 إطار/ثانية) لطلب إعادة الرسم. يعيد جدولة نفسه عبر
 * glutTimerFunc ليستمر. */
static void timer_cb(int val) {
    (void)val;
    glutPostRedisplay();                    /* ask GLUT to call display_cb | اطلب من GLUT الرسم */
    glutTimerFunc(40, timer_cb, 0);         /* reschedule after 40ms | أعد جدولة نفسي */
}

/* Display child entry point. Called once after fork. Initialises
 * GLUT, creates the window, and enters glutMainLoop (no return).
 *
 * نقطة الدخول لعملية العرض. تُستدعى مرة واحدة في الطفل بعد fork.
 * تهيّئ GLUT، تنشئ النافذة، وتدخل في glutMainLoop (لا ترجع). */
void graphics_run(SharedState *state) {
    g_st = state;                           /* save pointer for callbacks | احفظ المؤشر للـ callbacks */

    /* GLUT needs argc/argv but we pass none.
     * GLUT تحتاج argc/argv لكن لا نمرر أي arguments فعلية. */
    int argc = 0;
    glutInit(&argc, NULL);

    /* Display mode: double buffer + RGB color
     * نمط العرض: double buffer و RGB color */
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(1100, 500);          /* default window size | أبعاد النافذة الافتراضية */
    glutInitWindowPosition(100, 100);       /* start position | موقع البداية */
    glutCreateWindow("Furniture Competition");

    /* Register callbacks | سجّل الـ callbacks */
    glutDisplayFunc(display_cb);            /* for drawing | للرسم */
    glutTimerFunc(40, timer_cb, 0);         /* periodic refresh | لتحديث الإطار */

    /* Enter the GLUT main loop (never returns until killed).
     * ادخل الحلقة الرئيسية لـ GLUT (لا ترجع حتى يُقتل البرنامج). */
    glutMainLoop();
}
