#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* States in a thread's life cycle. */
enum thread_status
{
    THREAD_RUNNING, /* Running thread. */
    THREAD_READY,   /* Not running but ready to run. */
    THREAD_BLOCKED, /* Waiting for an event to trigger. */
    THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */


struct thread
{
    /* Owned by thread.c. */
    tid_t tid;                 /* Thread identifier. */
    enum thread_status status; /* Thread state. */
    char name[16];             /* Name (for debugging purposes). */
    uint8_t *stack;            /* Saved stack pointer. */
    int priority;              /* Priority. */
    struct list_elem allelem;  /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem; /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir; /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic; /* Detects stack overflow. */
 int original_priority;           /* 원래 우선순위 저장 */
    struct list donations;           /* 나에게 우선순위를 기부한 스레드들 */
    struct list_elem donation_elem;  /* donations 리스트에 들어가는 나 자신의 요소 */
    struct lock *wait_on_lock;       /* 내가 기다리고 있는 락 */
    int age;     
    int mlfqs_level;      /* MLFQS 큐 레벨 (0=최상위, 2=최하위) */
    int ticks_in_slice;
    int64_t wakeup_tick;         
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

bool thread_compare_donation_priority (const struct list_elem *a,
                                       const struct list_elem *b,
                                       void *aux);
void thread_update_priority (struct thread *t);
void thread_remove_donations (struct lock *lock);

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);
/* Alarm clock 관련 함수 선언 */
void thread_sleep (int64_t ticks);
void thread_wakeup (int64_t ticks);
int64_t get_next_tick_to_wakeup (void);
void update_next_tick_to_wakeup (int64_t ticks);

#endif /* threads/thread.h */
