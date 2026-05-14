/* ============================================================
 * main.c
 * ------------------------------------------------------------
 * Program entry point. Responsibilities:
 *   1. Read settings from config.txt
 *   2. Create System V shared memory and semaphore set
 *   3. Install signal handlers (SIGUSR1, SIGUSR2, SIGINT)
 *   4. Fork the OpenGL display child
 *   5. Round loop: for each round
 *        - generate fresh serials (Floyd's algorithm)
 *        - create forward pipes and backward FIFOs
 *        - fork all members (2 * team_size processes)
 *        - release the start barrier so both teams begin
 *        - wait until SIGUSR1/SIGUSR2 from a placer arrives
 *        - kill all members and clean up FIFOs
 *   6. Announce the champion and clean up IPC
 *
 * نقطة دخول البرنامج. مسؤولياتها:
 *   1. قراءة الإعدادات من ملف الإعدادات
 *   2. إنشاء الذاكرة المشتركة ومجموعة السيمافورات
 *   3. تركيب معالجات الإشارات الثلاث
 *   4. تفريع عملية العرض البصري
 *   5. حلقة الجولات: لكل جولة
 *        - توليد أرقام تسلسلية جديدة بخوارزمية فلويد
 *        - إنشاء الأنابيب للأمام والأنابيب المسماة للخلف
 *        - تفريع كل الأعضاء (2 * team_size عملية)
 *        - تحرير حاجز الانطلاق ليبدأ الفريقان معاً
 *        - الانتظار حتى يصل SIGUSR1 أو SIGUSR2 من أحد الواضعين
 *        - قتل كل الأعضاء وتنظيف الأنابيب المسماة
 *   6. إعلان البطل وتنظيف موارد الـ IPC
 * ============================================================ */

#include <stdio.h>          /* printf, fprintf, perror     | الطباعة والإعلام بالأخطاء */
#include <stdlib.h>         /* exit, rand, srand           | الخروج والعشوائية */
#include <string.h>         /* memset, memcpy              | عمليات الذاكرة */
#include <unistd.h>         /* fork, pipe, close, getpid   | استدعاءات النظام الأساسية */
#include <signal.h>         /* signal handlers, kill       | الإشارات */
#include <fcntl.h>          /* O_RDONLY, O_WRONLY          | أعلام الفتح */
#include <sys/stat.h>       /* mknod, S_IFIFO              | إنشاء الأنبوب المسمى */
#include <sys/wait.h>       /* waitpid                     | انتظار الأبناء */
#include <sys/ipc.h>        /* IPC_PRIVATE, IPC_CREAT      | ثوابت اتصال العمليات */
#include <sys/shm.h>        /* shmget, shmat, shmdt, shmctl| دوال الذاكرة المشتركة */
#include <sys/sem.h>        /* semget, semop, semctl       | دوال السيمافورات */
#include <time.h>           /* time for srand seed         | وقت بذرة العشوائية */
#include <omp.h>            /* OpenMP parallel for         | معالجة متوازية */

#include "common.h"
#include "config.h"
#include "member.h"
#include "graphics.h"

/* sigset is a System V signal API; we declare its prototype
 * manually because it isn't visible by default without
 * _XOPEN_SOURCE. Matches the style of the reference files.
 *
 * sigset هي واجهة سيستم في للإشارات. نصرّح عن نموذجها يدوياً
 * لأنها غير مرئية افتراضياً بدون _XOPEN_SOURCE. هذا التصريح
 * يطابق نمط الملفات المرجعية. */
typedef void (*sighandler_t)(int);
sighandler_t sigset(int sig, sighandler_t disp);

/* ------------------ globals | المتغيرات العامة ------------------ */

/* Pointer to shared memory after shmat.
 * مؤشر إلى الذاكرة المشتركة بعد ربطها بـ shmat. */
static SharedState              *g_shared;

/* IPC IDs; -1 means uninitialised.
 * معرّفات اتصال العمليات. سالب واحد يعني غير مهيأة بعد. */
static int                       g_shm_id;       /* shared memory segment | معرّف الذاكرة المشتركة */
static int                       g_sem_id;       /* semaphore set         | معرّف مجموعة السيمافورات */

/* Flag set inside a signal handler to indicate the round ended.
 * sig_atomic_t guarantees atomic read/write across handlers.
 *
 * علم يُضبط داخل معالج الإشارة للدلالة على انتهاء الجولة.
 * النوع sig_atomic_t يضمن قراءة وكتابة ذرية عبر المعالجات. */
static volatile sig_atomic_t     g_round_finished = 0;

/* Index of the team that won the current round; -1 = undecided.
 * رقم الفريق الفائز بالجولة الحالية. سالب واحد يعني لم يحسم. */
static volatile sig_atomic_t     g_winning_team   = -1;

/* ============================================================
 * Signal Handlers | معالجات الإشارات
 * ============================================================ */

/* SIGUSR1 handler: team 1's placer has finished the round.
 * We use two distinct signals (USR1 and USR2) because sigset
 * does not deliver siginfo_t, so we can't read sender PID.
 *
 * معالج إشارة SIGUSR1: واضع الفريق الأول أنهى الجولة. نستخدم
 * إشارتين مختلفتين لأن sigset لا تنقل معلومات المرسل، فبالإشارة
 * نفسها نعرف أي فريق فاز. */
static void on_team1_finished(int sig) {
    (void)sig;                                  /* ignore param | تجاهل الباراميتر */
    if (!g_round_finished) {                    /* first to report wins | الأول يفوز */
        g_winning_team   = 0;                   /* team 1 (index 0) | الفريق الأول */
        g_round_finished = 1;                   /* mark done | علم الانتهاء */
    }
}

/* SIGUSR2 handler: team 2's placer has finished the round.
 *
 * معالج إشارة SIGUSR2: واضع الفريق الثاني أنهى الجولة. */
static void on_team2_finished(int sig) {
    (void)sig;
    if (!g_round_finished) {
        g_winning_team   = 1;                   /* team 2 (index 1) | الفريق الثاني */
        g_round_finished = 1;
    }
}

/* SIGTERM handler used by the display child to exit cleanly
 * when the parent shuts down.
 *
 * معالج إشارة SIGTERM في عملية العرض. عند انتهاء البرنامج
 * يرسل الأب هذه الإشارة لعملية العرض لإغلاقها بسلام. */
static void on_terminate(int sig) {
    (void)sig;
    _exit(0);                                   /* fast exit | خروج فوري */
}

/* ============================================================
 * Utility Functions | دوال مساعدة
 * ============================================================ */

/* qsort comparator for ints in ascending order.
 *
 * دالة المقارنة المستخدمة مع qsort للترتيب التصاعدي للأعداد. */
static int compare_ints(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* Build the round's piece list using Floyd's sampling
 * algorithm, then make a sorted copy that the placer uses as
 * the expected order.
 *
 * Floyd's algorithm picks n unique values from [1, N] in O(n)
 * time without materialising or shuffling the full pool. Each
 * subset of size n is produced with equal probability. The
 * key invariant: at iteration j, the value j itself has not
 * been chosen yet, so it can be added unchecked when t collides.
 *
 * يولّد قائمة أرقام تسلسلية للجولة باستخدام خوارزمية فلويد،
 * ثم ينشئ نسخة مرتبة تصاعدياً يستخدمها الواضع كترتيب القبول.
 *
 * خوارزمية فلويد تختار n عنصراً فريداً من المجال [1, N] في
 * زمن خطّي O(n) فقط، دون تجسيد أو خلط الكومة الكاملة. كل
 * مجموعة جزئية بحجم n تُنتَج بنفس الاحتمال. الثابت المهم:
 * في التكرار رقم j القيمة j نفسها لم تُختر بعد، لذا يمكن
 * إضافتها مباشرة عند تصادم t. */
static void prepare_round_pieces(const Config *cfg, SharedState *shared) {
    int n_pieces = cfg->n_pieces;

    /* If user supplied serials, copy them as-is.
     * إذا وفّر المستخدم أرقاماً يدوياً ننسخها كما هي. */
    if (cfg->n_provided >= n_pieces) {
        memcpy(shared->raw_serials, cfg->provided_serials,
               (size_t)n_pieces * sizeof(int));
    } else {
        /* Sample pool = 5x the number of pieces (gives variety in numbers).
         * بركة العينات تساوي خمسة أضعاف عدد القطع لإعطاء تنوع. */
        int pool_size = n_pieces * 5;
        bool taken[MAX_PIECES * 5 + 1];         /* membership tracker | علامة الأخذ لكل رقم */

        /* === OpenMP parallel reset of the tracker array ===
         * Used to satisfy the project's OpenMP requirement.
         * The static schedule splits work evenly across threads.
         *
         * === تصفير المصفوفة بشكل متوازٍ باستخدام OpenMP ===
         * نستخدمها هنا لتلبية متطلب المشروع.
         * التوزيع الثابت يقسم العمل بالتساوي على الخيوط. */
        #pragma omp parallel for schedule(static)
        for (int i = 0; i <= pool_size; i++) taken[i] = false;

        /* === Floyd's algorithm core loop ===
         * === الحلقة الرئيسية لخوارزمية فلويد === */
        int out = 0;
        for (int j = pool_size - n_pieces + 1; j <= pool_size; j++) {
            int t = 1 + rand() % j;             /* random in [1, j] | عشوائي في المجال */
            if (taken[t]) {
                /* Collision: t already taken. Add j (not yet chosen by invariant).
                 * تصادم: t مأخوذ. أضف j بدلاً منه (لم يُختر بعد). */
                shared->raw_serials[out++] = j;
                taken[j] = true;
            } else {
                /* t is free: use it.
                 * t متاح: استخدمه. */
                shared->raw_serials[out++] = t;
                taken[t] = true;
            }
        }
    }

    /* Make sorted copy = placer's expected order.
     * أنشئ نسخة مرتبة تصاعدياً = ترتيب القبول عند الواضع. */
    memcpy(shared->sorted_serials, shared->raw_serials,
           (size_t)n_pieces * sizeof(int));
    qsort(shared->sorted_serials, (size_t)n_pieces, sizeof(int), compare_ints);
}

/* Create the team_size-1 backward-channel FIFOs for one team.
 * Each FIFO connects two adjacent members. Path includes pid
 * to avoid collisions with concurrent runs. Uses mknod (System
 * V style) instead of mkfifo.
 *
 * ينشئ team_size-1 من الأنابيب المسماة للقناة العائدة لفريق
 * واحد. كل أنبوب يربط عضوين متجاورين. الاسم يتضمن pid لتجنب
 * التعارض مع تشغيل آخر. يستخدم mknod (نمط سيستم في) بدلاً
 * من mkfifo. */
static void create_result_fifos(int team, int team_size, char paths[][64]) {
    for (int i = 0; i < team_size - 1; i++) {
        snprintf(paths[i], 64, "/tmp/rt_bwd_t%d_%d_%d", team, i, getpid());
        unlink(paths[i]);                       /* remove leftovers from earlier runs | احذف بقايا تشغيل سابق */
        if (mknod(paths[i], S_IFIFO | 0666, 0) < 0) {
            perror("mknod");                    /* print error message | اطبع رسالة الخطأ */
            exit(1);
        }
    }
}

/* Remove the backward-channel FIFOs for one team. Cleans /tmp.
 *
 * يحذف الأنابيب المسماة لفريق واحد لتنظيف مجلد /tmp. */
static void delete_result_fifos(int team, int team_size) {
    char path[64];
    for (int i = 0; i < team_size - 1; i++) {
        snprintf(path, sizeof(path), "/tmp/rt_bwd_t%d_%d_%d", team, i, getpid());
        unlink(path);                           /* delete from filesystem | احذف من نظام الملفات */
    }
}

/* Detach and remove the System V shared memory and semaphore
 * set. Must be called before exit; otherwise the resources
 * leak in the kernel. Also called from on_interrupt on Ctrl+C.
 *
 * يحرر موارد الذاكرة المشتركة ومجموعة السيمافورات. يجب
 * استدعاؤها قبل الخروج وإلا تبقى الموارد في النواة. تُستخدم
 * أيضاً من معالج المقاطعة عند الضغط على Ctrl+C. */
static void release_shared_resources(void) {
    if (g_shared) {
        shmdt(g_shared);                        /* detach segment | فك الربط عن العملية */
        g_shared = NULL;
    }
    /* IPC_RMID marks the segment for deletion when last process detaches.
     * IPC_RMID يعلّم المنطقة للحذف عند فك ربط آخر عملية. */
    if (g_shm_id >= 0) shmctl(g_shm_id, IPC_RMID, 0);
    if (g_sem_id >= 0) semctl(g_sem_id, 0, IPC_RMID, 0);
    g_shm_id = g_sem_id = -1;
}

/* SIGINT handler (Ctrl+C): release IPC then exit.
 *
 * معالج إشارة SIGINT (Ctrl+C): يحرر موارد الـ IPC قبل الخروج
 * لئلا تبقى في النظام. */
static void on_interrupt(int sig) {
    (void)sig;
    release_shared_resources();
    _exit(1);
}

/* Inside each forked child after fork: figure out which pipe
 * FDs this member uses, close the rest, then dispatch to the
 * matching role function:
 *   position == 0           -> run_picker
 *   position == team_size-1 -> run_placer
 *   anything in between     -> run_carrier
 *
 * تُستدعى داخل كل عملية ابن بعد التفريع لتحدد دور العضو ضمن
 * الفريق. تحدد الـ pipe FDs التي يستخدمها العضو، تغلق الباقي،
 * ثم توزّع على الدالة المناسبة:
 *   إذا كان أول عضو فهو الجالب
 *   إذا كان آخر عضو فهو الواضع
 *   أي عضو آخر فهو ناقل في الوسط. */
static void dispatch_team_member(int team, int position, int team_size, int n_pieces,
                                 int forward_pipes[][2], char result_paths[][64],
                                 const Config *cfg, SharedState *shared, int sem_id,
                                 pid_t parent_pid)
{
    /* forward_in: which FD to read from (if not the first member).
     * forward_in: من أي FD أقرأ (إن لم أكن أول عضو). */
    int forward_in  = (position > 0)              ? forward_pipes[position - 1][0] : -1;

    /* forward_out: which FD to write to (if not the last member).
     * forward_out: في أي FD أكتب (إن لم أكن آخر عضو). */
    int forward_out = (position < team_size - 1)  ? forward_pipes[position][1]     : -1;

    /* Close every other pipe FD this member doesn't own
     * (needed for proper EOF propagation along the chain).
     *
     * أغلق جميع الـ pipe FDs الأخرى التي لا تخص هذا العضو
     * (ضرورية لانتشار EOF عبر السلسلة). */
    for (int i = 0; i < team_size - 1; i++) {
        if (forward_pipes[i][0] != forward_in)  close(forward_pipes[i][0]);
        if (forward_pipes[i][1] != forward_out) close(forward_pipes[i][1]);
    }

    /* Dispatch to the appropriate role | وزّع على الدور المناسب */
    if (position == 0) {
        /* First member: picker | العضو الأول: الجالب */
        run_picker(team, n_pieces,
                   cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                   forward_out, result_paths[0], shared, sem_id);

    } else if (position == team_size - 1) {
        /* Last member: placer | العضو الأخير: الواضع */
        run_placer(team, n_pieces,
                   cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                   forward_in, result_paths[team_size - 2],
                   shared, sem_id, parent_pid);

    } else {
        /* Middle member: carrier. Reads verdict from next, sends to prev:
         *   result_in_path  = result_paths[position]      (from member position+1)
         *   result_out_path = result_paths[position-1]    (to member position-1)
         *
         * عضو في الوسط: ناقل. يقرأ النتيجة من التالي ويرسل للسابق:
         *   result_in_path  = result_paths[position]      من العضو position+1
         *   result_out_path = result_paths[position-1]    إلى العضو position-1 */
        run_carrier(team, position,
                    cfg->min_pause_ms, cfg->max_pause_ms, cfg->pause_increment_ms,
                    forward_in, forward_out,
                    result_paths[position], result_paths[position - 1],
                    shared, sem_id);
    }
}

/* ============================================================
 * main: program entry point | نقطة دخول البرنامج
 * ============================================================ */
int main(int argc, char *argv[]) {
    /* Pick config filename from argv or default to "config.txt".
     * استخراج اسم ملف الإعدادات من argv أو استخدام الافتراضي. */
    const char *cfg_file = (argc > 1) ? argv[1] : "config.txt";
    Config cfg;

    /* Load settings; if the file is missing, warn and use defaults.
     * حمّل الإعدادات. إذا فشل حذّر المستخدم واستخدم الافتراضي. */
    if (config_load(cfg_file, &cfg) < 0)
        fprintf(stderr, "Config file '%s' not found - using defaults.\n", cfg_file);

    config_print(&cfg);                         /* print config for confirmation | اطبع للتأكيد */

    /* Initialise globals before any step.
     * تهيئة المتغيرات العامة قبل أي خطوة. */
    g_shm_id = g_sem_id = -1;
    g_shared = NULL;

    /* ========== Create System V Shared Memory ==========
     * shmget creates a new segment of size sizeof(SharedState):
     *   IPC_PRIVATE: private key (not shareable with other programs)
     *   IPC_CREAT  : create if it does not exist
     *   0666       : read/write permissions for everyone
     *
     * ========== إنشاء الذاكرة المشتركة (سيستم في) ==========
     * shmget تنشئ منطقة جديدة بحجم sizeof(SharedState):
     *   IPC_PRIVATE: مفتاح خاص (غير قابل للمشاركة مع برامج أخرى)
     *   IPC_CREAT  : أنشئها إذا لم تكن موجودة
     *   0666       : صلاحيات قراءة وكتابة للجميع */
    g_shm_id = shmget(IPC_PRIVATE, sizeof(SharedState), IPC_CREAT | 0666);
    if (g_shm_id < 0) { perror("shmget"); exit(1); }

    /* shmat attaches the segment into the process's virtual
     * address space. The pointer is inherited automatically by
     * children when fork() is called.
     *
     * shmat تربط المنطقة إلى فضاء العنوان الافتراضي للعملية.
     * المؤشر يُورَّث تلقائياً للأبناء عند استدعاء fork. */
    g_shared = (SharedState *)shmat(g_shm_id, NULL, 0);
    if (g_shared == (void *)-1) { perror("shmat"); exit(1); }

    /* Initialise shared memory contents.
     * تهيئة محتوى الذاكرة المشتركة. */
    memset(g_shared, 0, sizeof(*g_shared));         /* zero everything | صفّر كل شيء */
    g_shared->n_members     = cfg.n_members;
    g_shared->n_pieces      = cfg.n_pieces;
    g_shared->n_wins_needed = cfg.n_wins_needed;
    g_shared->winner_team   = -1;                    /* no winner yet | لم يحسم الفائز بعد */
    g_shared->transit_serial[0] = g_shared->transit_serial[1] = -1;  /* no piece in transit | لا قطعة في النقل */

    /* ========== Create System V Semaphore Set ==========
     * Two semaphores for the start barrier:
     *   sem[0] = SEM_ARRIVE -> counts arriving members
     *   sem[1] = SEM_DEPART -> releases all members at once
     *
     * ========== إنشاء مجموعة السيمافورات (سيستم في) ==========
     * سيمافوران لحاجز الانطلاق:
     *   sem[0] = SEM_ARRIVE -> يعدّ الأعضاء الواصلين
     *   sem[1] = SEM_DEPART -> يحرّر الجميع دفعة واحدة */
    g_sem_id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0666);
    if (g_sem_id < 0) { perror("semget"); release_shared_resources(); exit(1); }

    /* ========== Install signal handlers ==========
     * Use sigset (System V) as in the reference code.
     *
     * ========== تركيب معالجات الإشارات ==========
     * استخدام sigset (سيستم في) كما في الكود المرجعي. */
    sigset(SIGUSR1, on_team1_finished);             /* team 1 win | فوز الفريق الأول */
    sigset(SIGUSR2, on_team2_finished);             /* team 2 win | فوز الفريق الثاني */
    sigset(SIGINT,  on_interrupt);                  /* Ctrl+C: cleanup then exit | تنظيف ثم خروج */

    /* Seed the random generator once in the parent.
     * تهيئة بذرة العشوائية مرة واحدة في الأب. */
    srand((unsigned)time(NULL));

    /* Flush buffers before fork so the child doesn't replay the parent's output.
     * تفريغ المخازن قبل التفريع لئلا يطبع الطفل ما طبعه الأب. */
    fflush(stdout);
    fflush(stderr);

    /* ========== Fork the display child ==========
     * ========== تفريع عملية العرض البصري ========== */
    pid_t display_pid = fork();
    if (display_pid < 0) { perror("fork display"); release_shared_resources(); exit(1); }
    if (display_pid == 0) {
        /* Inside the display process | داخل عملية العرض */
        fclose(stdin);                          /* close stdin (not needed) | أغلق stdin */
        sigset(SIGTERM, on_terminate);          /* respond to SIGTERM with quick exit | استجب بخروج سريع */
        graphics_run(g_shared);                 /* enter glutMainLoop (never returns) | لا يرجع */
        _exit(0);
    }

    int team_size = cfg.n_members;              /* shorthand | اختصار */

    /* ============================================================
     * Round loop: repeats until one team reaches n_wins_needed.
     * حلقة الجولات: تستمر حتى يصل أحد الفرق إلى n_wins_needed.
     * ============================================================ */
    for (int round = 1; ; round++) {
        printf("\n=== Round %d starting ===\n", round);
        fflush(stdout);

        /* ===== Set up the new round | إعداد الجولة الجديدة ===== */
        prepare_round_pieces(&cfg, g_shared);   /* generate fresh serials | ولّد أرقاماً جديدة */
        g_shared->current_round     = round;
        g_shared->pieces_placed[0]  = 0;        /* reset team 1 counter | عداد الفريق الأول */
        g_shared->pieces_placed[1]  = 0;        /* reset team 2 counter | عداد الفريق الثاني */
        g_shared->transit_serial[0] = g_shared->transit_serial[1] = -1;
        g_round_finished            = 0;        /* round not over yet | الجولة لم تنتهِ */
        g_winning_team              = -1;

        /* Reset both barrier semaphores to 0 for this round.
         * أعد تهيئة سيمافوري الحاجز إلى صفر لهذه الجولة. */
        union semun arg;
        arg.val = 0;
        semctl(g_sem_id, SEM_ARRIVE, SETVAL, arg);
        semctl(g_sem_id, SEM_DEPART, SETVAL, arg);

        /* ===== Forward-direction pipes (anonymous) =====
         * Per team we need team_size-1 pipes to link adjacent members.
         *
         * ===== الأنابيب للأمام (مجهولة الاسم) =====
         * لكل فريق نحتاج team_size-1 من الأنابيب لربط الأعضاء المتجاورين. */
        int forward_team1[MAX_MEMBERS][2], forward_team2[MAX_MEMBERS][2];
        for (int i = 0; i < team_size - 1; i++) {
            if (pipe(forward_team1[i]) < 0 || pipe(forward_team2[i]) < 0) {
                perror("pipe"); release_shared_resources(); exit(1);
            }
        }

        /* ===== Backward-direction FIFOs (named) =====
         * ===== الأنابيب المسماة للخلف ===== */
        char result_team1[MAX_MEMBERS][64], result_team2[MAX_MEMBERS][64];
        create_result_fifos(0, team_size, result_team1);    /* team 1 | الفريق الأول */
        create_result_fifos(1, team_size, result_team2);    /* team 2 | الفريق الثاني */

        /* ===== Fork all members of both teams =====
         * ===== تفريع كل أعضاء الفريقين ===== */
        pid_t worker_pids[2 * MAX_MEMBERS];     /* store child pids | احفظ pids الأبناء */
        int   n_workers = 0;                    /* child counter   | عداد الأبناء */
        pid_t parent_pid = getpid();            /* save parent pid for children | احفظ pid الأب */

        /* Fork team 1 members | تفريع أعضاء الفريق الأول */
        for (int i = 0; i < team_size; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork team1"); release_shared_resources(); exit(1); }
            if (pid == 0) {
                /* Inside child: run role then exit | داخل الطفل: شغّل الدور ثم اخرج */
                dispatch_team_member(0, i, team_size, cfg.n_pieces,
                                     forward_team1, result_team1,
                                     &cfg, g_shared, g_sem_id, parent_pid);
                _exit(0);
            }
            worker_pids[n_workers++] = pid;     /* parent: save child pid | احفظ pid الطفل */
        }

        /* Fork team 2 members | تفريع أعضاء الفريق الثاني */
        for (int i = 0; i < team_size; i++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork team2"); release_shared_resources(); exit(1); }
            if (pid == 0) {
                dispatch_team_member(1, i, team_size, cfg.n_pieces,
                                     forward_team2, result_team2,
                                     &cfg, g_shared, g_sem_id, parent_pid);
                _exit(0);
            }
            worker_pids[n_workers++] = pid;
        }

        /* Parent closes all pipe FDs (children own them now).
         * الأب يغلق كل الـ pipe FDs (الأبناء يستخدمونها الآن). */
        for (int i = 0; i < team_size - 1; i++) {
            close(forward_team1[i][0]); close(forward_team1[i][1]);
            close(forward_team2[i][0]); close(forward_team2[i][1]);
        }

        /* ===== Start barrier: release both teams together =====
         *   1. Wait for all 2*team_size members to raise SEM_ARRIVE.
         *      semop(-2*team_size) blocks until value reaches that.
         *   2. Raise SEM_DEPART by +(2*team_size) in one atomic semop
         *      so all members are released at the same instant.
         *
         * ===== حاجز الانطلاق: تحرير الفريقين معاً =====
         *   1. ننتظر حتى يصل جميع الأعضاء (2*team_size) ويرفعوا SEM_ARRIVE.
         *      semop بقيمة -(2*team_size) يُحجَب حتى تصبح القيمة كافية.
         *   2. ثم نرفع SEM_DEPART بـ +(2*team_size) دفعة واحدة فيتحرر
         *      الجميع في نفس اللحظة (atomic semop). */
        struct sembuf op;
        op.sem_num = SEM_ARRIVE;
        op.sem_op  = -(short)(2 * team_size);   /* wait for all arrivals | انتظر وصول الجميع */
        op.sem_flg = 0;
        if (semop(g_sem_id, &op, 1) < 0) {
            perror("semop arrive"); release_shared_resources(); exit(1);
        }

        op.sem_num = SEM_DEPART;
        op.sem_op  = +(short)(2 * team_size);   /* release all together | حرّر الجميع معاً */
        op.sem_flg = 0;
        if (semop(g_sem_id, &op, 1) < 0) {
            perror("semop depart"); release_shared_resources(); exit(1);
        }

        /* Parent waits for SIGUSR1/SIGUSR2 from a placer.
         * pause() returns on any signal; we re-check the flag.
         *
         * الأب ينتظر إشارة من أحد الواضعين (SIGUSR1 أو SIGUSR2).
         * الدالة pause ترجع عند أي إشارة فنفحص علم الانتهاء. */
        while (!g_round_finished) pause();

        int winner = (int)g_winning_team;       /* round winner team | الفريق الفائز بالجولة */

        usleep(300000);                         /* let display show last frame | امنح العرض 300ms */

        /* Kill all children and reap them | اقتل جميع الأبناء وانتظرهم */
        for (int i = 0; i < n_workers; i++) kill(worker_pids[i], SIGTERM);
        for (int i = 0; i < n_workers; i++) waitpid(worker_pids[i], NULL, 0);

        /* Remove FIFOs | احذف الأنابيب المسماة */
        delete_result_fifos(0, team_size);
        delete_result_fifos(1, team_size);

        /* Record and announce the round result.
         * سجّل وأعلن نتيجة الجولة. */
        g_shared->team_wins[winner]++;
        printf("Team %d wins round %d!  Score: Team1=%d  Team2=%d\n",
               winner + 1, round,
               g_shared->team_wins[0], g_shared->team_wins[1]);
        fflush(stdout);

        /* Stop if a team reached the wins target.
         * تحقق هل وصل أحد الفرق للعدد المطلوب من الفوز. */
        if (g_shared->team_wins[0] >= cfg.n_wins_needed ||
            g_shared->team_wins[1] >= cfg.n_wins_needed) {
            break;
        }

        sleep(1);                               /* short pause between rounds | استراحة قصيرة */
    }

    /* ===== Announce the champion | إعلان البطل النهائي ===== */
    int champion = (g_shared->team_wins[0] >= cfg.n_wins_needed) ? 0 : 1;
    g_shared->winner_team = champion;           /* tell the display | اكتبها للعرض */
    g_shared->game_over   = 1;                  /* show banner in display | أظهر banner النهاية */

    printf("\n*** Team %d wins the competition! ***\n", champion + 1);
    printf("Final score:  Team 1 = %d   Team 2 = %d\n\n",
           g_shared->team_wins[0], g_shared->team_wins[1]);
    fflush(stdout);

    sleep(4);                                   /* let display show final state | امنح العرض وقتاً */

    /* Stop the display child and wait for it.
     * أوقف عملية العرض وانتظرها. */
    kill(display_pid, SIGTERM);
    waitpid(display_pid, NULL, 0);

    /* Final IPC cleanup | تنظيف الـ IPC قبل الخروج */
    release_shared_resources();
    return 0;
}
