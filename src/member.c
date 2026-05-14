/* ============================================================
 * member.c
 * ------------------------------------------------------------
 * Implementation of the three team-member roles:
 *   1. run_picker  - first member, grabs pieces from pile
 *   2. run_carrier - middle members, relay pieces and verdicts
 *   3. run_placer  - last member, places pieces in the house
 *
 * Each function runs in its own forked child. Communication is
 * via:
 *   - pipes for forward piece transit (anonymous)
 *   - FIFOs for backward verdict transit (named, mknod)
 *   - shared memory for visualization counters
 *   - semaphores for the start barrier (synchronised launch)
 *   - signals to inform the parent about round completion
 *
 * تنفيذ الأدوار الثلاثة لأعضاء الفريق:
 *   1. run_picker  - العضو الأول، يجلب القطع من الكومة
 *   2. run_carrier - أعضاء الوسط، ينقلون القطع والنتائج
 *   3. run_placer  - العضو الأخير، يضع القطع في البيت
 *
 * كل دالة تعمل في عملية ابن مفروقة بشكل مستقل. التواصل عبر:
 *   - الأنابيب للقطع المتجهة للأمام (مجهولة الاسم)
 *   - الأنابيب المسماة للنتائج العائدة (mknod)
 *   - الذاكرة المشتركة لتحديث العرض البصري والعدادات
 *   - السيمافورات لحاجز الانطلاق (الانطلاق المتزامن)
 *   - الإشارات لإعلام الأب بانتهاء الجولة
 * ============================================================ */

#include <stdio.h>          /* perror | إعلام بالأخطاء */
#include <stdlib.h>         /* rand, srand | العشوائية */
#include <string.h>         /* memset | عمليات الذاكرة */
#include <unistd.h>         /* read, write, close, getpid, usleep | استدعاءات النظام */
#include <signal.h>         /* kill, signal handlers | الإشارات */
#include <fcntl.h>          /* O_RDONLY, O_WRONLY | أعلام الفتح */
#include <time.h>           /* time for srand | بذرة العشوائية */
#include <sys/ipc.h>        /* IPC constants | ثوابت اتصال العمليات */
#include <sys/sem.h>        /* semop, struct sembuf | السيمافورات */
#include "member.h"

/* sigset prototype declared manually (System V style).
 *
 * تصريح يدوي لـ sigset (نمط سيستم في). */
typedef void (*sighandler_t)(int);
sighandler_t sigset(int sig, sighandler_t disp);

/* ============================================================
 * Static Helpers | دوال مساعدة
 * ============================================================ */

/* Sleep for a fatigue-scaled duration:
 *   base = min_ms + moves * inc_ms   (linear growth)
 *   then base is clamped to max_ms   (fatigue ceiling)
 *   then a random jitter in [0, headroom] is added
 * The growth is linear (NOT exponential): the worker starts
 * fast and rests longer as moves accumulate, until the ceiling.
 *
 * تنفّذ راحة بطول متناسب مع عدد الحركات السابقة (إرهاق):
 *   base = min_ms + moves * inc_ms   (نمو خطّي)
 *   ثم يُحَد base عند max_ms          (سقف الإرهاق)
 *   ثم يُضاف jitter عشوائي في [0, headroom]
 * النمو خطّي (وليس أسّياً): العضو يبدأ سريعاً وينام أكثر
 * كلما تراكمت حركاته حتى يصل إلى السقف. */
static void rest_with_fatigue(int min_ms, int max_ms, int inc_ms, int moves) {
    int base = min_ms + moves * inc_ms;             /* linear base | القاعدة الخطّية */
    if (base > max_ms) base = max_ms;               /* clamp ceiling | لا تتجاوز السقف */
    int headroom = max_ms - base;                   /* room for jitter | مساحة للعشوائية */
    int jitter   = (headroom > 0) ? (rand() % (headroom + 1)) : 0;
    usleep((useconds_t)(base + jitter) * 1000);     /* usleep takes microsec | usleep تأخذ ميكروثانية */
}

/* Write a complete PipeMsg to fd, looping until done.
 * Uses a loop because write() may return short.
 * Returns 0 on success, -1 if the pipe is broken.
 *
 * يكتب رسالة PipeMsg كاملة على الـ fd، باستخدام حلقة لأن write
 * قد يكتب أقل من المطلوب (partial write). يعيد صفر عند النجاح،
 * أو سالب واحد عند الفشل (مثلاً انكسار الأنبوب). */
static int send_message(int fd, const PipeMsg *msg) {
    size_t done = 0;                                /* bytes written so far | عدد البايتات المكتوبة */
    const char *p = (const char *)msg;              /* byte pointer | مؤشر بايت */
    while (done < sizeof(*msg)) {
        ssize_t n = write(fd, p + done, sizeof(*msg) - done);
        if (n <= 0) return -1;                      /* error or EOF | خطأ أو نهاية */
        done += (size_t)n;                          /* advance | حدّث العداد */
    }
    return 0;
}

/* Read a complete PipeMsg from fd, looping until done.
 * Same idea as send_message but for read.
 *
 * يقرأ رسالة PipeMsg كاملة من الـ fd. نفس فكرة send_message
 * لكن للقراءة. */
static int receive_message(int fd, PipeMsg *msg) {
    size_t done = 0;
    char *p = (char *)msg;
    while (done < sizeof(*msg)) {
        ssize_t n = read(fd, p + done, sizeof(*msg) - done);
        if (n <= 0) return -1;                      /* EOF or error | نهاية أو خطأ */
        done += (size_t)n;
    }
    return 0;
}

/* Open a FIFO and retry until it succeeds.
 * open() on a FIFO with O_WRONLY/O_RDONLY blocks until the
 * other end is opened. The loop guards against EINTR.
 *
 * يفتح أنبوباً مسمى ويعيد المحاولة حتى ينجح. الدالة open على
 * الأنبوب المسمى تحجب حتى يفتح الطرف الآخر. الحلقة تحمي من
 * EINTR (مقاطعة من إشارة). */
static int open_fifo_blocking(const char *path, int flags) {
    int fd;
    do { fd = open(path, flags); } while (fd < 0);
    return fd;
}

/* Two-step semaphore barrier:
 *   1. SEM_ARRIVE += 1   announce arrival
 *   2. SEM_DEPART -= 1   wait for parent's release
 *
 * Why two steps? The parent must know "everyone has arrived"
 * before releasing; otherwise it might release before some
 * members reach the barrier and lose synchronisation.
 *   - Parent waits for SEM_ARRIVE >= 2*team_size via semop(-2*team_size)
 *   - Then raises SEM_DEPART by +(2*team_size) in one atomic semop
 *   - All members release at the exact same instant
 * This guarantees fairness: no team starts before the other
 * because of fork order.
 *
 * تنفّذ حاجز الانطلاق على شكل خطوتين:
 *   1. SEM_ARRIVE += 1   أعلن أنني وصلت
 *   2. SEM_DEPART -= 1   انتظر تحرير الأب
 *
 * لماذا خطوتين؟ لأن الأب يجب أن يعرف "متى وصل الجميع" قبل أن
 * يحرر، وإلا قد يحرر قبل وصول البعض فلا تحدث المزامنة الحقيقية.
 *   - الأب ينتظر SEM_ARRIVE >= 2*team_size عبر semop(-2*team_size)
 *   - ثم يرفع SEM_DEPART بـ +(2*team_size) في عملية ذرية واحدة
 *   - فيتحرر الجميع في نفس اللحظة بالضبط
 * هذا يضمن العدالة: لا فريق ينطلق قبل الآخر بسبب ترتيب fork. */
static void wait_at_starting_line(int sem_id) {
    struct sembuf op;

    /* Step 1: announce arrival | الخطوة الأولى: أعلن الوصول */
    op.sem_num = SEM_ARRIVE;
    op.sem_op  = +1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);

    /* Step 2: wait for the parent to release | الخطوة الثانية: انتظر الإطلاق */
    op.sem_num = SEM_DEPART;
    op.sem_op  = -1;
    op.sem_flg = 0;
    semop(sem_id, &op, 1);
}

/* ============================================================
 * Picker: member 0 - grabs pieces from the pile
 * الجالب: العضو رقم صفر - يجلب القطع من الكومة
 * ============================================================ */

/* The picker:
 *   1. Open the FIFO coming from member 1 (for verdicts)
 *   2. Wait at the start barrier
 *   3. Loop:
 *        - pick a random piece from available, non-rejected ones
 *        - update transit info in shared memory for the display
 *        - rest (fatigue)
 *        - send the piece forward through the pipe
 *        - wait for the verdict on the FIFO
 *        - if accepted: mark as placed, clear rejected list
 *        - if rejected: add to rejected list, retry later
 *
 * الجالب مهمته:
 *   1. يفتح الأنبوب المسمى القادم من العضو رقم واحد (للنتائج)
 *   2. ينتظر عند حاجز الانطلاق
 *   3. حلقة:
 *        - يختار قطعة عشوائية من المتاحة وغير المرفوضة
 *        - يحدّث transit info في الذاكرة المشتركة للعرض
 *        - ينام (إرهاق)
 *        - يرسل القطعة عبر الأنبوب للأمام
 *        - ينتظر النتيجة (verdict) من الأنبوب المسمى العائد
 *        - إذا قُبلت: يعلّمها كموضوعة ويصفّر قائمة المرفوضة
 *        - إذا رُفضت: يضعها في قائمة المرفوضة (سيعيد المحاولة لاحقاً) */
void run_picker(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_out, const char *result_in_path,
                SharedState *shared, int sem_id)
{
    /* Different random seed per child | بذرة عشوائية مختلفة لكل ابن */
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 8));

    /* Ignore SIGPIPE so a broken pipe doesn't kill us; we
     * handle it via return values instead.
     *
     * تجاهل SIGPIPE لئلا يقتلنا انكسار الأنبوب. نتعامل معه
     * عبر القيمة المرجعة بدلاً من ذلك. */
    sigset(SIGPIPE, SIG_IGN);

    /* Open the FIFO for reading from the next member.
     * افتح الأنبوب المسمى للقراءة من العضو التالي. */
    int result_in = open_fifo_blocking(result_in_path, O_RDONLY);

    /* Wait with the rest of the members at the starting line.
     * انتظر مع باقي الأعضاء عند نقطة البداية. */
    wait_at_starting_line(sem_id);

    /* Track each piece's status:
     *   available[i] = has it not been placed yet?
     *   rejected[i]  = was it rejected this round of attempts?
     * Rejected pieces are skipped temporarily, then given a new
     * chance whenever any other piece is accepted.
     *
     * مصفوفتان لتتبع حالة كل قطعة:
     *   available[i] = هل لم تُوضع بعد؟
     *   rejected[i]  = هل رُفضت في هذه المحاولات؟
     * المرفوضة تُجنّب مؤقتاً ثم تُعطى فرصة جديدة عند نجاح أي قطعة. */
    bool available[MAX_PIECES], rejected[MAX_PIECES];
    for (int i = 0; i < n_pieces; i++) { available[i] = true; rejected[i] = false; }

    int placed = 0, moves = 0;                  /* placed counter & move counter | عدّاد الموضوعات والحركات */

    while (placed < n_pieces) {
        /* Build the list of pickable indices (available and not rejected).
         * اجمع قائمة الـ indexes القابلة للاختيار (متاحة وغير مرفوضة). */
        int pickable[MAX_PIECES], n_pickable = 0;
        for (int i = 0; i < n_pieces; i++)
            if (available[i] && !rejected[i]) pickable[n_pickable++] = i;

        /* If everything left has been rejected, retry by clearing the set.
         * إذا كان كل المتبقي مرفوضاً نعيد المحاولة بمسح القائمة. */
        if (n_pickable == 0) {
            for (int i = 0; i < n_pieces; i++) rejected[i] = false;
            for (int i = 0; i < n_pieces; i++)
                if (available[i]) pickable[n_pickable++] = i;
        }

        /* Pick a random index from the pickable list.
         * اختر index عشوائياً من القائمة. */
        int chosen_idx = pickable[rand() % n_pickable];
        int serial     = shared->raw_serials[chosen_idx];   /* actual piece serial | رقم القطعة الفعلي */

        /* Update shared memory so the display knows what's happening.
         * حدّث الذاكرة المشتركة ليعرف العرض البصري ما يحدث. */
        shared->transit_serial[team] = serial;
        shared->transit_member[team] = 0;       /* I am the picker (#0) | أنا الجالب رقم صفر */
        shared->transit_dir[team]    = 1;       /* forward direction | الاتجاه للأمام */

        /* Rest (fatigue) then send | ارتح (إرهاق) ثم أرسل */
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        PipeMsg outgoing = { serial, 0 };       /* accepted=0 (no meaning here) | لا معنى لها هنا */
        if (send_message(forward_out, &outgoing) < 0) break;    /* pipe broken | انكسر الأنبوب */

        /* Wait for the verdict to come back through the FIFO.
         * انتظر النتيجة العائدة عبر الأنبوب المسمى. */
        PipeMsg verdict;
        if (receive_message(result_in, &verdict) < 0) break;

        /* Handle the verdict | تعامل مع النتيجة */
        if (verdict.accepted) {
            available[chosen_idx] = false;      /* never pick again | لن تُختار مرة أخرى */
            memset(rejected, 0, n_pieces * sizeof(bool));   /* clear rejected list | صفّر المرفوضات */
            placed++;
            shared->pieces_placed[team] = placed;   /* announce progress to display | أعلن النجاح للعرض */
        } else {
            rejected[chosen_idx] = true;        /* set aside for now | جنبها مؤقتاً */
        }
    }

    /* Round done from my side - clear transit marker.
     * انتهت الجولة من جانبي - أزل علامة النقل. */
    shared->transit_serial[team] = -1;
    close(result_in);
    close(forward_out);
}

/* ============================================================
 * Carrier: middle members - relay forward and backward
 * الناقل: أعضاء الوسط - ينقلون للأمام والخلف
 * ============================================================ */

/* The carrier:
 *   1. Open the result-write FIFO (to previous member) FIRST,
 *      then the result-read FIFO (from next member). The order
 *      matters! If everyone opened read first, you'd get a
 *      chain deadlock waiting for the next one to open write.
 *   2. Wait at the start barrier
 *   3. Loop:
 *        - read a piece from the previous pipe
 *        - update transit, rest (fatigue)
 *        - send to the next pipe
 *        - read the verdict from the next FIFO
 *        - update transit (reverse direction), rest
 *        - send to the previous FIFO
 *
 * الناقل مهمته:
 *   1. يفتح أولاً FIFO الكتابة (للعضو السابق) ثم FIFO القراءة
 *      (من التالي). الترتيب مهم! لو فتح الكل القراءة أولاً
 *      يحدث deadlock في السلسلة.
 *   2. ينتظر حاجز الانطلاق
 *   3. حلقة:
 *        - يقرأ قطعة من الأنبوب السابق
 *        - يحدّث transit ثم ينام (إرهاق)
 *        - يرسلها للأنبوب التالي
 *        - يقرأ النتيجة من الأنبوب المسمى القادم من التالي
 *        - يحدّث transit (اتجاه عكسي) ثم ينام
 *        - يرسلها للأنبوب المسمى المتجه للسابق */
void run_carrier(int team, int position,
                 int min_ms, int max_ms, int inc_ms,
                 int forward_in, int forward_out,
                 const char *result_in_path,
                 const char *result_out_path,
                 SharedState *shared, int sem_id)
{
    /* Different seed using pid and position | بذرة مختلفة باستخدام pid و position */
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 4) ^ (unsigned)position);
    sigset(SIGPIPE, SIG_IGN);

    /* IMPORTANT: open the write end before the read end to
     * break the open() chain deadlock. If every carrier opened
     * the read first, each would block waiting for the next to
     * open write -> nobody starts. Opening write first breaks
     * the chain.
     *
     * مهم جداً: افتح طرف الكتابة قبل طرف القراءة لكسر سلسلة
     * الـ deadlock. لو كل ناقل فتح القراءة أولاً، كل واحد
     * سينتظر التالي يفتح الكتابة -> deadlock. الكتابة أولاً
     * تكسر هذه السلسلة. */
    int result_out = open_fifo_blocking(result_out_path, O_WRONLY);
    int result_in  = open_fifo_blocking(result_in_path,  O_RDONLY);

    wait_at_starting_line(sem_id);

    int moves = 0;

    for (;;) {
        /* === Trip 1: piece coming from previous, going to next ===
         * === الرحلة الأولى: قطعة قادمة من السابق ومتجهة للتالي === */
        PipeMsg piece;
        if (receive_message(forward_in, &piece) < 0) break;     /* no more pieces | لا قطع جديدة */

        shared->transit_member[team] = position;    /* I'm holding the piece | أنا أحمل القطعة */
        shared->transit_dir[team]    = 1;           /* forward | اتجاه أمامي */
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        if (send_message(forward_out, &piece) < 0) break;       /* send to next | أرسل للتالي */

        /* === Trip 2: verdict coming from next, going to previous ===
         * === الرحلة الثانية: نتيجة قادمة من التالي ومتجهة للسابق === */
        PipeMsg verdict;
        if (receive_message(result_in, &verdict) < 0) break;

        shared->transit_member[team] = position;
        shared->transit_dir[team]    = -1;          /* backward | اتجاه خلفي */
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        if (send_message(result_out, &verdict) < 0) break;
    }

    close(forward_in);
    close(forward_out);
    close(result_in);
    close(result_out);
}

/* ============================================================
 * Placer: last member - places pieces in the house
 * الواضع: العضو الأخير - يضع القطع في البيت
 * ============================================================ */

/* The placer:
 *   1. Open the FIFO for writing (to the previous member)
 *   2. Wait at the start barrier
 *   3. Loop until all pieces are placed:
 *        - receive a piece from the previous pipe
 *        - update transit and rest (fatigue)
 *        - check: does serial == sorted_serials[expected_idx] ?
 *            yes -> verdict.accepted = 1, expected_idx++
 *            no  -> verdict.accepted = 0  (send back rejected)
 *        - send the verdict via FIFO to the previous member
 *   4. After all pieces placed: send SIGUSR1 (team 0) or
 *      SIGUSR2 (team 1) to the parent so it knows we won.
 *
 * الواضع مهمته:
 *   1. يفتح الأنبوب المسمى للكتابة (للعضو قبل الأخير - يرسل النتائج عبره)
 *   2. ينتظر حاجز الانطلاق
 *   3. حلقة (حتى تُوضع كل القطع):
 *        - يستلم قطعة من الأنبوب السابق
 *        - يحدّث transit وينام (إرهاق)
 *        - يفحص: هل serial == sorted_serials[expected_idx] ؟
 *            نعم -> verdict.accepted = 1 و expected_idx++
 *            لا  -> verdict.accepted = 0  (إعادتها)
 *        - يرسل النتيجة عبر الأنبوب المسمى للعضو السابق
 *   4. عند انتهاء كل القطع: يرسل SIGUSR1 إن كان الفريق الأول
 *      أو SIGUSR2 إن كان الفريق الثاني، إلى الأب ليعلن نهاية الجولة. */
void run_placer(int team, int n_pieces,
                int min_ms, int max_ms, int inc_ms,
                int forward_in, const char *result_out_path,
                SharedState *shared, int sem_id, pid_t parent_pid)
{
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 2));
    sigset(SIGPIPE, SIG_IGN);

    /* Open FIFO for writing (to previous member).
     * افتح الأنبوب المسمى للكتابة (للعضو السابق). */
    int result_out = open_fifo_blocking(result_out_path, O_WRONLY);

    wait_at_starting_line(sem_id);

    int expected_idx = 0;                       /* index of next expected serial | index الرقم التالي المتوقع */
    int moves        = 0;

    while (expected_idx < n_pieces) {
        /* Receive a piece from the chain | استلم قطعة من السلسلة */
        PipeMsg piece;
        if (receive_message(forward_in, &piece) < 0) break;

        /* Display: I (last member) am holding it; will return it shortly.
         * العرض البصري: أنا (آخر عضو) أحمل القطعة، اتجاه عودتها للخلف قريباً. */
        shared->transit_member[team] = shared->n_members - 1;
        shared->transit_dir[team]    = -1;
        rest_with_fatigue(min_ms, max_ms, inc_ms, moves++);

        /* Default verdict: rejected | جهّز النتيجة الافتراضية: مرفوضة */
        PipeMsg verdict = { piece.serial, 0 };

        /* Accept only if it matches the next expected serial in sorted order.
         * اقبلها فقط إذا طابقت الرقم المتوقع التالي بحسب الترتيب التصاعدي. */
        if (piece.serial == shared->sorted_serials[expected_idx]) {
            verdict.accepted = 1;
            expected_idx++;                     /* move to next expected | انتقل للرقم التالي */
        }

        /* Send verdict (accepted/rejected) back to previous member.
         * أرسل النتيجة (مقبولة أو مرفوضة) للعضو السابق. */
        if (send_message(result_out, &verdict) < 0) break;
    }

    /* Round done from my side - clear transit marker.
     * انتهت الجولة من جانبي - أزل علامة النقل. */
    shared->transit_serial[team] = -1;

    /* Tell the parent: SIGUSR1 if I'm team 0, SIGUSR2 if team 1.
     * We don't use siginfo because sigset doesn't deliver it,
     * so the signal number itself identifies the team.
     *
     * أعلن للأب: SIGUSR1 لو أنا الفريق الأول، SIGUSR2 لو الفريق
     * الثاني. لا نستخدم siginfo لأن sigset لا تنقلها، فالإشارة
     * نفسها تحدد الفريق. */
    kill(parent_pid, (team == 0) ? SIGUSR1 : SIGUSR2);

    close(forward_in);
    close(result_out);
}
