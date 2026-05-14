/* ============================================================
 * config.c
 * ------------------------------------------------------------
 * Implementation of the config-file reader. Parses key=value
 * lines from a text file and fills a Config struct, applying
 * sane clamps to out-of-range values.
 *
 * تنفيذ قارئ ملف الإعدادات. يحلّل سطور "مفتاح يساوي قيمة" من
 * ملف نصي ويعبّئ بنية Config، ثم يحصر القيم خارج المدى المعقول
 * إلى حدود سليمة.
 * ============================================================ */

#include <stdio.h>      /* fopen, fgets, printf, fprintf, fclose | دوال الملفات والطباعة */
#include <stdlib.h>     /* atoi                                  | تحويل النص إلى رقم */
#include <string.h>     /* strcmp, strchr, strtok, strlen        | دوال النصوص */
#include <ctype.h>      /* isspace                               | فحص الحرف الفارغ */
#include "config.h"

/* Strip leading and trailing whitespace from s in place and
 * return a pointer to the new start.
 *   1. advance s while the char is whitespace (skip front)
 *   2. find end e, retreat while previous char is whitespace
 *   3. terminate the string at the new end
 *
 * تزيل المسافات من بداية ونهاية النص في مكانه وترجع مؤشراً
 * للبداية الجديدة:
 *   أولاً: تتقدم s طالما الحرف مسافة لتجاوز المسافات الأمامية
 *   ثانياً: تجد النهاية e وتتراجع طالما الحرف قبلها مسافة
 *   ثالثاً: تكتب صفر منهي في e لقطع النص. */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;                     /* skip leading | تخطي الأمامية */
    char *e = s + strlen(s);                                    /* end pointer  | مؤشر النهاية */
    while (e > s && isspace((unsigned char)*(e - 1))) e--;      /* trim trailing | قص الخلفية */
    *e = '\0';                                                  /* terminate    | إنهاء النص */
    return s;
}

/* Populate cfg with the built-in default values.
 *
 * تضع القيم الافتراضية في cfg. */
void config_defaults(Config *cfg) {
    cfg->n_members          = 4;     /* 4 members per team default | أربعة أعضاء افتراضياً */
    cfg->n_pieces           = 20;    /* 20 pieces per round        | عشرون قطعة في الجولة */
    cfg->n_wins_needed      = 3;     /* 3 wins to win the game     | ثلاث جولات للفوز */
    cfg->min_pause_ms       = 10;    /* 10ms minimum rest          | عشر ميلي ثواني أقل راحة */
    cfg->max_pause_ms       = 200;   /* 200ms maximum rest         | 200 ميلي ثانية سقف الإرهاق */
    cfg->pause_increment_ms = 3;     /* 3ms extra per move         | ثلاث ميلي ثوان لكل حركة */
    cfg->n_provided         = 0;     /* no manual serials -> random | لا أرقام محددة (عشوائي) */
}

/* Read the configuration file line by line and fill cfg.
 * Returns 0 on success, -1 if the file cannot be opened.
 *
 * يقرأ ملف الإعدادات سطراً سطراً ويعبّئ cfg.
 * يعيد صفر عند النجاح، أو سالب واحد إذا فشل فتح الملف. */
int config_load(const char *path, Config *cfg) {
    config_defaults(cfg);                       /* always start from defaults | ابدأ بالافتراضي دائماً */

    FILE *f = fopen(path, "r");                 /* open for reading | افتح للقراءة */
    if (!f) return -1;                          /* file missing -> use defaults | الملف غير موجود */

    char line[1024];                            /* buffer for one line | مخزن سطر واحد */
    while (fgets(line, sizeof(line), f)) {      /* read line by line | اقرأ سطراً سطراً */
        char *p = trim(line);                   /* strip whitespace | أزل المسافات */
        if (!*p || *p == '#') continue;         /* skip blank/comment | تخطّى الفارغ والتعليق */

        char *eq = strchr(p, '=');              /* find '=' | ابحث عن علامة المساواة */
        if (!eq) continue;                      /* no '=' -> skip | بدون = تخطّى */
        *eq = '\0';                             /* split key/value | اقطع لفصل المفتاح والقيمة */

        char *key = trim(p);                    /* key = before '=' | المفتاح ما قبل = */
        char *val = trim(eq + 1);               /* val = after '='  | القيمة ما بعد = */

        /* Match the key against each known option | قارن المفتاح مع كل خيار معروف */
        if      (strcmp(key, "n_members")          == 0) cfg->n_members          = atoi(val);
        else if (strcmp(key, "n_pieces")            == 0) cfg->n_pieces           = atoi(val);
        else if (strcmp(key, "n_wins_needed")       == 0) cfg->n_wins_needed      = atoi(val);
        else if (strcmp(key, "min_pause_ms")        == 0) cfg->min_pause_ms       = atoi(val);
        else if (strcmp(key, "max_pause_ms")        == 0) cfg->max_pause_ms       = atoi(val);
        else if (strcmp(key, "pause_increment_ms")  == 0) cfg->pause_increment_ms = atoi(val);
        else if (strcmp(key, "serials") == 0) {
            /* Comma-separated list: serials = 5, 12, 7, 3, ... |
             * قائمة أرقام مفصولة بفواصل مثل 5، 12، 7، 3 ... */
            char *tok = strtok(val, ",");
            while (tok && cfg->n_provided < MAX_PIECES) {
                cfg->provided_serials[cfg->n_provided++] = atoi(trim(tok));
                tok = strtok(NULL, ",");        /* continue same string | استكمل نفس النص */
            }
        }
    }
    fclose(f);                                  /* close file | أغلق الملف */

    /* === Clamp out-of-range values to sane bounds ===
     * === حصر القيم خارج المدى المعقول إلى حدود سليمة === */
    if (cfg->n_members < 2)            cfg->n_members = 2;             /* min 2 members  | اثنان كحد أدنى */
    if (cfg->n_members > MAX_MEMBERS)  cfg->n_members = MAX_MEMBERS;   /* max members    | الحد الأقصى */
    if (cfg->n_pieces  < 1)            cfg->n_pieces  = 1;             /* at least 1     | قطعة واحدة على الأقل */
    if (cfg->n_pieces  > MAX_PIECES)   cfg->n_pieces  = MAX_PIECES;    /* max pieces     | الحد الأقصى */

    /* If user gave fewer serials than needed, fall back to random.
     *
     * إذا حُددت أرقام يدوية أقل من المطلوب نرجع للعشوائي. */
    if (cfg->n_provided > 0 && cfg->n_provided < cfg->n_pieces) {
        fprintf(stderr, "Warning: %d serials provided but %d pieces needed - "
                        "switching to random.\n", cfg->n_provided, cfg->n_pieces);
        cfg->n_provided = 0;
    }

    return 0;
}

/* Print the configuration to stdout in a tidy format.
 *
 * يطبع الإعدادات على المخرج القياسي بشكل منسّق. */
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
