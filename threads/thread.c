/* threads/thread.c */

#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h" // timer_ticks() 사용 위해 추가

#ifdef USERPROG
#include "userprog/process.h"
#endif

/* 디버그 출력 활성화 (문제가 해결되면 주석 처리) */
// #define MLFQS_DEBUG

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* [MLFQS FIX] MLFQS용 Ready 큐 3개 (Q0, Q1, Q2) */
static struct list mlfqs_ready_queues[3];

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List of process in sleep */
static struct list sleep_list;
static int64_t next_tick_to_wakeup = INT64_MAX;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread (FOR PRIORITY SCHEDULER). */
static unsigned thread_ticks;   /* # of timer ticks since last yield (FOR PRIORITY SCHEDULER). */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* 스레드 우선순위 비교 함수 (list_insert_ordered 용) */
bool
thread_priority_less_func (const struct list_elem *a,
                           const struct list_elem *b,
                           void *aux UNUSED)
{
    struct thread *thread_a = list_entry (a, struct thread, elem);
    struct thread *thread_b = list_entry (b, struct thread, elem);
    return thread_a->priority > thread_b->priority;
}

/* Initializes the threading system. */
void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    list_init (&ready_list); // Priority scheduler's ready list
    list_init (&all_list);
    list_init (&sleep_list);
    
    /* Initialize MLFQS queues if enabled */
    if (thread_mlfqs)
    {
        list_init (&mlfqs_ready_queues[0]);
        list_init (&mlfqs_ready_queues[1]);
        list_init (&mlfqs_ready_queues[2]);
    }
    
    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread ();
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
    /* Create the idle thread. */
    struct semaphore idle_started;
    sema_init (&idle_started, 0);
    thread_create ("idle", PRI_MIN, idle, &idle_started);

    /* Start preemptive thread scheduling. */
    intr_enable ();

    /* Wait for the idle thread to initialize idle_thread. */
    sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick. */
void
thread_tick (void)
{
    struct thread *t = thread_current ();

    /* Update statistics. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    if (thread_mlfqs)
    {
#ifdef MLFQS_DEBUG
        if (t != idle_thread) {
             printf("Tick %lld: Run='%s' (L%d, T%d, A%d)\n", 
                    timer_ticks(), t->name, t->mlfqs_level, t->ticks_in_slice + 1, t->age); // +1 because tick increments after this
        }
#endif
        /* --- MLFQS Logic --- */
        
        /* Increment ticks_in_slice for the running thread (if not idle) */
        if (t != idle_thread)
            t->ticks_in_slice++;

        /* 1. Promotion (Aging): Increment age for ALL threads (ready, blocked/sleeping) */
        
        /* 1-1. Sleep List */
        struct list_elem *e = list_begin (&sleep_list);
        while (e != list_end (&sleep_list))
        {
            struct thread *s_t = list_entry (e, struct thread, elem);
            s_t->age++;
            if (s_t->age >= 20)
            {
                s_t->age = 0;
                if (s_t->mlfqs_level > 0)
                    s_t->mlfqs_level--; /* Promoted level applied upon wakeup */
            }
            e = list_next(e);
        }

        /* 1-2. Ready Queues (Q1, Q2 only, as Q0 cannot promote) */
        for (int i = 1; i <= 2; i++)
        {
            e = list_begin (&mlfqs_ready_queues[i]);
            while (e != list_end (&mlfqs_ready_queues[i]))
            {
                struct thread *r_t = list_entry (e, struct thread, elem);
                r_t->age++;

                if (r_t->age >= 20)
                {
                    struct list_elem *next_e = list_next(e); 
#ifdef MLFQS_DEBUG
                    printf("Tick %lld: PROMOTE '%s' from L%d to L%d\n", 
                           timer_ticks(), r_t->name, r_t->mlfqs_level, r_t->mlfqs_level - 1);
#endif                   
                    r_t->age = 0;
                    r_t->mlfqs_level--; /* Promote */
                    
                    /* Reset slice on promotion - Assumption */
                    r_t->ticks_in_slice = 0; 
                    
                    list_remove(&r_t->elem); /* Remove from current queue */
                    list_push_back(&mlfqs_ready_queues[r_t->mlfqs_level], &r_t->elem); /* Add to promoted queue */
                    
                    e = next_e; /* Move to next element safely */
                }
                else
                {
                    e = list_next (e); /* Move to next element */
                }
            }
        }
        
        /* 2. Demotion: Check if current thread used its time slice */
        int slice_limit = 0;
        if (t->mlfqs_level == 0) slice_limit = 2;
        else if (t->mlfqs_level == 1) slice_limit = 4;
        else if (t->mlfqs_level == 2) slice_limit = 8;

        if (t != idle_thread && t->ticks_in_slice >= slice_limit)
        {
#ifdef MLFQS_DEBUG
            printf("Tick %lld: DEMOTE '%s' from L%d to L%d\n", 
                   timer_ticks(), t->name, t->mlfqs_level, 
                   (t->mlfqs_level < 2 ? t->mlfqs_level + 1 : 2) );
#endif
            t->ticks_in_slice = 0; /* Reset slice counter */
            t->age = 0;            /* Reset age on demotion */
            if (t->mlfqs_level < 2)
                t->mlfqs_level++;  /* Demote to lower queue */
            
            intr_yield_on_return(); /* Yield CPU after demotion */
        }

        /* 3. Preemption: Check if higher priority thread exists */
        if (t != idle_thread) 
        {
            bool yield_for_preempt = false;
            /* If running in Q1, check Q0 */
            if (t->mlfqs_level == 1 && !list_empty(&mlfqs_ready_queues[0]))
            {
                yield_for_preempt = true;
            }
            /* If running in Q2, check Q0 and Q1 */
            else if (t->mlfqs_level == 2)
            {
                if (!list_empty(&mlfqs_ready_queues[0]) || !list_empty(&mlfqs_ready_queues[1]))
                   yield_for_preempt = true;
            }
            
            if (yield_for_preempt) {
#ifdef MLFQS_DEBUG
                 printf("Tick %lld: PREEMPT '%s' (L%d) yields for higher queue\n", 
                        timer_ticks(), t->name, t->mlfqs_level);
#endif
                 intr_yield_on_return(); /* Yield CPU if higher priority thread ready */
            }
        }
    }
    else /* --- Priority + Aging Logic --- */
    {
        struct list_elem *e = list_begin (&ready_list);
        while (e != list_end (&ready_list))
        {
            struct thread *ready_t = list_entry (e, struct thread, elem);
            ready_t->age++;

            if (ready_t->age >= 20)
            {
                if (ready_t->priority < PRI_MAX)
                {
                    ready_t->priority++;
                }
                ready_t->age = 0;
                
                struct list_elem *next_e = list_next(e);
                list_remove(&ready_t->elem);
                list_insert_ordered(&ready_list, &ready_t->elem, thread_priority_less_func, NULL);
                e = next_e;
            }
            else
            {
                e = list_next (e); 
            }
        }

        /* Enforce preemption using standard TIME_SLICE for priority scheduler */
        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return ();
            
    } 
}
