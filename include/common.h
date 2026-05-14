#ifndef COMMON_H
#define COMMON_H

/* ============================================================
 * common.h
 * ------------------------------------------------------------
 * Shared header file used by every translation unit in the
 * project. Provides the System V IPC includes, hard limits,
 * the union semun definition (missing on most Linux distros),
 * and the structs that live in shared memory.
 *
 * ملف الترويسة المشترك بين جميع وحدات الترجمة في المشروع.
 * يوفّر مكتبات اتصال العمليات من نوع سيستم في،
 * والحدود القصوى، وتعريف الاتحاد semun الناقص في معظم
 * توزيعات لينكس، والبُنى التي تعيش في الذاكرة المشتركة.
 * ============================================================ */

#include <sys/types.h>      /* basic types like pid_t and key_t | الأنواع الأساسية */
#include <sys/ipc.h>        /* IPC_PRIVATE, IPC_CREAT, IPC_RMID | الثوابت العامة لاتصال العمليات */
#include <sys/shm.h>        /* shmget, shmat, shmdt, shmctl    | دوال الذاكرة المشتركة */
#include <sys/sem.h>        /* semget, semop, semctl, sembuf   | دوال السيمافورات */
#include <stdbool.h>        /* the bool type                   | النوع المنطقي */

/* Maximum number of members in a single team. Used to size
 * fixed arrays so we avoid malloc.
 *
 * الحد الأقصى لعدد أعضاء الفريق الواحد. نستخدمه لحجز مصفوفات
 * ثابتة الحجم بدلاً من التخصيص الديناميكي. */
#define MAX_MEMBERS  32

/* Maximum number of furniture pieces per round.
 *
 * الحد الأقصى لعدد قطع الأثاث في الجولة الواحدة. */
#define MAX_PIECES   2000

/* Number of competing teams (always 2 per project spec).
 *
 * عدد الفرق المتنافسة (دائماً اثنان حسب متطلبات المشروع). */
#define MAX_TEAMS    2

/* Semaphore indices inside the start-barrier set:
 *   SEM_ARRIVE counts members reaching the starting line
 *   SEM_DEPART releases all members at the same instant
 *
 * أرقام السيمافورات داخل مجموعة حاجز الانطلاق:
 *   SEM_ARRIVE يعدّ الأعضاء الواصلين إلى نقطة البداية
 *   SEM_DEPART يحرّر جميع الأعضاء في نفس اللحظة */
enum { SEM_ARRIVE = 0, SEM_DEPART = 1 };

/* union semun is required by semctl but not declared in
 * <sys/sem.h> on most Linux distributions, so we declare it
 * manually (same trick used by parent.c / consumer.c).
 *
 * الاتحاد semun مطلوب من قبل semctl ولكنه غير معرّف في
 * <sys/sem.h> في معظم توزيعات لينكس، لذلك نعرّفه يدوياً
 * (نفس الحيلة المستخدمة في الكود المرجعي). */
union semun {
    int              val;       /* SETVAL value     | لتعيين قيمة سيمافور واحد */
    struct semid_ds *buf;       /* IPC_STAT buffer  | لقراءة حالة المجموعة */
    unsigned short  *array;     /* SETALL array     | لتعيين كل القيم دفعة واحدة */
};

/* Message format sent through the forward pipes and the
 * backward FIFOs. Size is fixed and small so reads/writes are
 * atomic within PIPE_BUF.
 *
 * صيغة الرسالة المرسلة عبر الأنابيب للأمام والأنابيب المسماة
 * للخلف. الحجم ثابت وصغير لتكون عمليات القراءة والكتابة ذرية
 * ضمن حدود PIPE_BUF. */
typedef struct {
    int serial;     /* furniture piece serial number | الرقم التسلسلي لقطعة الأثاث */
    int accepted;   /* 0 = rejected, 1 = accepted    | صفر مرفوضة، واحد مقبولة */
} PipeMsg;

/* Shared memory segment visible to every process: the parent,
 * all member processes after fork, and the display child.
 * Attached once in the parent before any fork; children
 * inherit the same pointer through virtual memory copy.
 *
 * منطقة ذاكرة مشتركة يراها كل العمليات: الأب، وجميع أعضاء
 * الفريقين بعد التفريع، وعملية العرض. تُربط مرة واحدة في
 * الأب قبل أي تفريع، فيرث الأبناء المؤشر نفسه عبر نسخ
 * الذاكرة الافتراضية. */
typedef struct {
    /* === static config (written once at startup) ===
     * === الإعدادات الثابتة (تُكتب مرة واحدة في البداية) === */
    int n_members;          /* members per team       | عدد الأعضاء في كل فريق */
    int n_pieces;           /* pieces per round       | عدد قطع الأثاث في الجولة */
    int n_wins_needed;      /* round wins to win game | عدد الجولات للفوز بالمنافسة */

    /* === current round state (mutates during play) ===
     * === حالة الجولة الحالية (تتغير أثناء اللعب) === */
    int current_round;                  /* current round number | رقم الجولة الحالية */
    int team_wins[MAX_TEAMS];           /* wins per team        | عدد الجولات لكل فريق */
    int pieces_placed[MAX_TEAMS];       /* placed this round    | الموضوعة هذه الجولة */
    int game_over;                      /* 1 when game ends     | واحد عند انتهاء المنافسة */
    int winner_team;                    /* -1 = undecided yet   | رقم الفريق الفائز */

    /* === visualization data (transit info) ===
     * === بيانات للعرض البصري === */
    int transit_serial[MAX_TEAMS];      /* moving serial, -1 none | رقم القطعة المتحركة */
    int transit_member[MAX_TEAMS];      /* member holding it      | العضو الذي يحملها */
    int transit_dir[MAX_TEAMS];         /* +1 forward, -1 back    | الاتجاه أمام أو خلف */

    /* === serial numbers for the current round ===
     * === الأرقام التسلسلية للجولة الحالية === */
    int raw_serials[MAX_PIECES];        /* unsorted, picker view  | كما يراها الجالب */
    int sorted_serials[MAX_PIECES];     /* sorted = placer order  | مرتبة = ترتيب القبول */
} SharedState;

/* Configuration loaded from config.txt or defaults.
 *
 * إعدادات البرنامج المقروءة من ملف الإعدادات أو الافتراضية. */
typedef struct {
    int n_members;          /* members per team           | عدد الأعضاء في كل فريق */
    int n_pieces;           /* pieces per round           | عدد القطع في الجولة */
    int n_wins_needed;      /* round wins to win game     | عدد جولات الفوز */
    int min_pause_ms;       /* min rest between moves     | أقل راحة بين الحركات */
    int max_pause_ms;       /* max rest (fatigue ceiling) | أعلى راحة (سقف الإرهاق) */
    int pause_increment_ms; /* extra rest per move        | الزيادة في الراحة لكل حركة */
    int provided_serials[MAX_PIECES];   /* user-supplied serials | أرقام محددة يدوياً */
    int n_provided;                     /* count, 0 = random     | صفر يعني عشوائي */
} Config;

#endif /* COMMON_H */
