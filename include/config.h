#ifndef CONFIG_H
#define CONFIG_H

/* ============================================================
 * config.h
 * ------------------------------------------------------------
 * Declares functions to load and print the program settings
 * from a text file. The file format is "key = value", and any
 * line that begins with '#' is treated as a comment.
 *
 * يصرّح عن دوال تحميل وطباعة إعدادات البرنامج من ملف نصي.
 * صيغة الملف هي "مفتاح يساوي قيمة"، وأي سطر يبدأ بعلامة المربع
 * يُعتبر تعليقاً يُتجاهل.
 * ============================================================ */

#include "common.h"

/* Load settings from a key=value text file. Returns 0 on
 * success, or -1 if the file could not be opened (defaults
 * remain populated in cfg in that case).
 *
 * يقرأ الإعدادات من ملف نصي بصيغة مفتاح يساوي قيمة. يعيد
 * صفر عند النجاح، وسالب واحد إذا فشل فتح الملف. في حال
 * الفشل تبقى cfg محتوية على القيم الافتراضية. */
int  config_load(const char *path, Config *cfg);

/* Populate cfg with the built-in default values. Always called
 * before parsing the file so missing keys keep sane values.
 *
 * يضع القيم الافتراضية في cfg. تُستدعى دائماً قبل قراءة الملف
 * لضمان قيم سليمة حتى لو فُقد بعض المفاتيح. */
void config_defaults(Config *cfg);

/* Print the current configuration to stdout for verification.
 *
 * يطبع الإعدادات الحالية على المخرج القياسي للتأكيد من قراءتها. */
void config_print(const Config *cfg);

#endif /* CONFIG_H */
