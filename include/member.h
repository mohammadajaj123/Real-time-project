#ifndef MEMBER_H
#define MEMBER_H

/* ============================================================
 * member.h
 * ------------------------------------------------------------
 * Declares the three role functions executed by team members.
 * Each function runs in its own forked child process.
 *   run_picker  : member 0,        grabs pieces from the pile
 *   run_carrier : members 1..N-2,  relay pieces and verdicts
 *   run_placer  : member N-1,      places pieces in the house
 *
 * Communication channels between members:
 *   forward  : anonymous pipes  (picker -> ... -> placer)
 *   backward : named FIFOs      (placer -> ... -> picker)
 *   barrier  : System V semaphore set (parent releases all)
 *
 * يصرّح عن دوال الأدوار الثلاثة لأعضاء الفريق.
 * كل دالة تعمل داخل عملية ابن مفروقة بشكل مستقل.
 *   run_picker  : العضو رقم صفر، يجلب القطع من الكومة
 *   run_carrier : الأعضاء من واحد إلى N-2، ينقلون القطع والنتائج
 *   run_placer  : العضو الأخير N-1، يضع القطع في البيت
 *
 * قنوات الاتصال بين الأعضاء:
 *   للأمام  : أنابيب مجهولة (من الجالب إلى الواضع)
 *   للخلف   : أنابيب مسماة عبر دالة mknod
 *   للحاجز  : مجموعة سيمافورات سيستم في يحررها الأب
 * ============================================================ */

#include "common.h"
#include <sys/types.h>      /* for pid_t | لنوع pid_t */

/* Picker (member 0):
 * Grab a random piece from the pile, send it forward through
 * forward_out, then wait for the verdict on the FIFO at
 * result_in_path. If accepted, mark the piece as placed; if
 * rejected, set it aside and try a different piece.
 *
 * الجالب (العضو رقم صفر):
 * يختار قطعة عشوائية من الكومة، يرسلها للأمام عبر forward_out،
 * ثم ينتظر النتيجة من الأنبوب المسمى عند result_in_path.
 * إذا قُبلت يحسبها كموضوعة، وإذا رُفضت يضعها في قائمة المرفوضة
 * مؤقتاً ويختار قطعة أخرى. */
void run_picker(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_out, const char *result_in_path,
                SharedState *shared, int sem_id);

/* Carrier (members 1..N-2):
 * Relay incoming pieces forward to the next member through a
 * pipe, then relay the verdict backward to the previous member
 * through a FIFO. Rest with growing fatigue between each step.
 *
 * الناقل (الأعضاء من واحد إلى N-2):
 * ينقل القطعة المستلمة من العضو السابق إلى العضو التالي عبر
 * الأنبوب، ثم ينقل النتيجة العائدة من التالي إلى السابق عبر
 * الأنبوب المسمى. يأخذ راحة متزايدة بين كل خطوة. */
void run_carrier(int team, int position,
                 int min_ms, int max_ms, int inc_ms,
                 int forward_in, int forward_out,
                 const char *result_in_path,
                 const char *result_out_path,
                 SharedState *shared, int sem_id);

/* Placer (member N-1):
 * Receive pieces, accept only the next expected serial in
 * sorted order, send a verdict (accepted or rejected) back.
 * When all pieces are placed, send SIGUSR1 (team 1) or SIGUSR2
 * (team 2) to the parent so it knows the round is over.
 *
 * الواضع (العضو الأخير N-1):
 * يستلم القطع، ويقبل فقط الرقم التالي المتوقع حسب الترتيب
 * التصاعدي، ويرسل النتيجة (مقبولة أو مرفوضة) للخلف. عند انتهاء
 * كل القطع يرسل SIGUSR1 إن كان الفريق الأول أو SIGUSR2 إن كان
 * الفريق الثاني، إلى الأب ليعلن انتهاء الجولة. */
void run_placer(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_in, const char *result_out_path,
                SharedState *shared, int sem_id, pid_t parent_pid);

#endif /* MEMBER_H */
