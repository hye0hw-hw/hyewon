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
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif
#define THREAD_MAGIC 0xcd6abf4b

static struct list ready_list;



static struct list mlfqs_ready_queues[3];




static struct list all_list;



static struct list sleep_list;

static int64_t next_tick_to_wakeup = INT64_MAX;




static struct thread *idle_thread;




static struct thread *initial_thread;




static struct lock tid_lock;




struct kernel_thread_frame

{

    void *eip;             /* Return address. */

    thread_func *function; /* Function to call. */

    void *aux;             /* Auxiliary data for function. */

};





static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */



#define TIME_SLICE 4          /* # of timer ticks to give each thread. */

static unsigned thread_ticks; /* # of timer ticks since last yield. */




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




bool

thread_priority_less_func (const struct list_elem *a,
                           const struct list_elem *b,
                           void *aux UNUSED)

{
    struct thread *thread_a = list_entry (a, struct thread, elem);
    struct thread *thread_b = list_entry (b, struct thread, elem);
    return thread_a->priority > thread_b->priority;

}


void
thread_init (void)
{
    ASSERT (intr_get_level () == INTR_OFF);

    lock_init (&tid_lock);
    list_init (&ready_list);
    list_init (&all_list);
    list_init (&sleep_list);


    if (thread_mlfqs)

    {
        list_init (&mlfqs_ready_queues[0]);
        list_init (&mlfqs_ready_queues[1]);
        list_init (&mlfqs_ready_queues[2]);

    }

    

   

    initial_thread = running_thread ();

    init_thread (initial_thread, "main", PRI_DEFAULT);

    initial_thread->status = THREAD_RUNNING;

    initial_thread->tid = allocate_tid ();

}





void
thread_start (void)

{
   
    struct semaphore idle_started;

    sema_init (&idle_started, 0);

    thread_create ("idle", PRI_MIN, idle, &idle_started);
    intr_enable ();
    sema_down (&idle_started);

}




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
       
        if (t != idle_thread)
            t->ticks_in_slice++;

        
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
    else 
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



/* Prints thread statistics. */

void

thread_print_stats (void)

{

    printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",

            idle_ticks, kernel_ticks, user_ticks);

}




tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)

{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;
    enum intr_level old_level;

    ASSERT (function != NULL);

    t = palloc_get_page (PAL_ZERO);

    if (t == NULL)

        return TID_ERROR;


    init_thread (t, name, priority);

    tid = t->tid = allocate_tid ();



   

    old_level = intr_disable ();



   

    kf = alloc_frame (t, sizeof *kf);

    kf->eip = NULL;

    kf->function = function;

    kf->aux = aux;



   

    ef = alloc_frame (t, sizeof *ef);

    ef->eip = (void (*) (void))kernel_thread;



  

    sf = alloc_frame (t, sizeof *sf);

    sf->eip = switch_entry;

    sf->ebp = 0;



    intr_set_level (old_level);



    thread_unblock (t);


    bool preempt = false;

    if (thread_mlfqs)

    {
        if (t->mlfqs_level < thread_current()->mlfqs_level)

            preempt = true;

    }

    else

    {

        if (t->priority > thread_current ()->priority)

            preempt = true;

    }



    if (preempt)

    {

        thread_yield ();

    }



    return tid;

}



void

thread_block (void)

{

    ASSERT (!intr_context ());

    ASSERT (intr_get_level () == INTR_OFF);



    thread_current ()->status = THREAD_BLOCKED;

    schedule ();

}




void
thread_unblock (struct thread *t)

{
    enum intr_level old_level;
    ASSERT (is_thread (t));



    old_level = intr_disable ();

    ASSERT (t->status == THREAD_BLOCKED);

    t->age = 0;

    

    if (thread_mlfqs)

    {

       

        list_push_back (&mlfqs_ready_queues[t->mlfqs_level], &t->elem);

    }

    else

    {
        list_insert_ordered (&ready_list, &t->elem, thread_priority_less_func, NULL);

    }

    

    

    t->status = THREAD_READY;

    intr_set_level (old_level);

    



}



static void

update_next_tick_to_wakeup (int64_t tick)

{

    next_tick_to_wakeup = 

        (next_tick_to_wakeup > tick) ? tick : next_tick_to_wakeup;

}



int64_t

get_next_tick_to_wakeup (void)

{

    return next_tick_to_wakeup;

}




void

thread_sleep (int64_t tick)

{

    struct thread *cur;

    enum intr_level old_level;



    old_level = intr_disable ();

    cur = thread_current ();



    ASSERT (cur != idle_thread);



    update_next_tick_to_wakeup (cur->wakeup_tick = tick);

    list_push_back (&sleep_list, &cur->elem);



    thread_block ();



    intr_set_level (old_level);

}



void

thread_wakeup (int64_t current_tick)

{

    struct list_elem *e;



    next_tick_to_wakeup = INT64_MAX;



    e = list_begin (&sleep_list);

    while (e != list_end (&sleep_list))

    {

        struct thread *t = list_entry (e, struct thread, elem);

        if (current_tick >= t->wakeup_tick)

        {

            e = list_remove (&t->elem);

            thread_unblock (t);

        }

        else

        {

            e = list_next (e);

            update_next_tick_to_wakeup (t->wakeup_tick);

        }

    }

}




const char *

thread_name (void)

{

    return thread_current ()->name;

}



/* Returns the running thread.

   This is running_thread() plus a couple of sanity checks.

   See the big comment at the top of thread.h for details. */

struct thread *

thread_current (void)

{

    struct thread *t = running_thread ();




    ASSERT (is_thread (t));

    ASSERT (t->status == THREAD_RUNNING);



    return t;

}



/* Returns the running thread's tid. */

tid_t

thread_tid (void)

{

    return thread_current ()->tid;

}




void

thread_exit (void)

{

    ASSERT (!intr_context ());



#ifdef USERPROG

    process_exit ();

#endif




    intr_disable ();

    list_remove (&thread_current ()->allelem);

    thread_current ()->status = THREAD_DYING;

    schedule ();

    NOT_REACHED ();

}





void

thread_yield (void)

{

    struct thread *cur = thread_current ();

    enum intr_level old_level;



    ASSERT (!intr_context ());



    old_level = intr_disable ();

    if (cur != idle_thread)

    {

        cur->age = 0; 

        

        if (thread_mlfqs)

        {

            /* [FIX 6-1] MLFQS: 큐의 '뒤'에 추가 (FIFO) */

            list_push_back (&mlfqs_ready_queues[cur->mlfqs_level], &cur->elem);

        }

        else

        {

            /* [FIX 6-2] Priority: 기존의 우선순위 정렬 큐에 추가 */

            list_insert_ordered (&ready_list, &cur->elem, thread_priority_less_func, NULL);

        }

    }

    cur->status = THREAD_READY;

    schedule ();

    intr_set_level (old_level);

}



/* Invoke function 'func' on all threads, passing along 'aux'.

   This function must be called with interrupts off. */

void

thread_foreach (thread_action_func *func, void *aux)

{

    struct list_elem *e;



    ASSERT (intr_get_level () == INTR_OFF);



    for (e = list_begin (&all_list); e != list_end (&all_list);

         e = list_next (e))

        {

            struct thread *t = list_entry (e, struct thread, allelem);

            func (t, aux);

        }

}



/* Sets the current thread's priority to NEW_PRIORITY. */

void

thread_set_priority (int new_priority)

{

    thread_current ()->priority = new_priority;



    /*

     * [FIX 2] 선점 로직은 Priority 스케줄러일 때만 동작해야 합니다.

     * (MLFQS는 이 함수로 우선순위를 설정하지 않으며, ready_list를 사용하지 않음)

     */

    if (!thread_mlfqs && !list_empty (&ready_list))

    {

        struct thread *highest_ready = 

            list_entry (list_front (&ready_list), struct thread, elem);



        /*

         * ready_list의 최고 우선순위 스레드보다

         * 현재 스레드의 우선순위가 낮다면, 즉시 양보합니다.

         */

        if (new_priority < highest_ready->priority)

        {

            thread_yield ();

        }

    }

}





/* Returns the current thread's priority. */

/* threads/thread.c */

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
    /* [FIX] MLFQS 활성화 시, mlfqs_level에 따라 우선순위 반환 */
    if (thread_mlfqs)
    {
        struct thread *cur = thread_current();
        if (cur->mlfqs_level == 0) {
            return PRI_MAX; // Q0 -> Highest priority
        } else if (cur->mlfqs_level == 1) {
            return PRI_DEFAULT; // Q1 -> Default priority
        } else { // cur->mlfqs_level == 2
            return PRI_MIN; // Q2 -> Lowest priority
        }
    }
    else /* Priority 스케줄러일 때는 기존 priority 값 반환 */
    {
       return thread_current ()->priority;
    }
}



/* Sets the current thread's nice value to NICE. */

void

thread_set_nice (int nice UNUSED)

{

    /* Not yet implemented. */

}



/* Returns the current thread's nice value. */

int

thread_get_nice (void)

{

    /* Not yet implemented. */

    return 0;

}



/* Returns 100 times the system load average. */

int

thread_get_load_avg (void)

{

    /* Not yet implemented. */

    return 0;

}



/* Returns 100 times the current thread's recent_cpu value. */

int

thread_get_recent_cpu (void)

{

    /* Not yet implemented. */

    return 0;

}



/* Idle thread.  Executes when no other thread is ready to run.



   The idle thread is initially put on the ready list by

   thread_start().  It will be scheduled once initially, at which

   point it initializes idle_thread, "up"s the semaphore passed

   to it to enable thread_start() to continue, and immediately

   blocks.  After that, the idle thread never appears in the

   ready list.  It is returned by next_thread_to_run() as a

   special case when the ready list is empty. */

static void

idle (void *idle_started_ UNUSED)

{

    struct semaphore *idle_started = idle_started_;

    idle_thread = thread_current ();

    sema_up (idle_started);



    for (;;)

        {

            /* Let someone else run. */

            intr_disable ();

            thread_block ();



            /* Re-enable interrupts and wait for the next one.



         The `sti' instruction disables interrupts until the

         completion of the next instruction, so these two

         instructions are executed atomically.  This atomicity is

         important; otherwise, an interrupt could be handled

         between re-enabling interrupts and waiting for the next

         one to occur, wasting as much as one clock tick worth of

         time.



         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]

         7.11.1 "HLT Instruction". */

            asm volatile ("sti; hlt" : : : "memory");

        }

}



/* Function used as the basis for a kernel thread. */

static void

kernel_thread (thread_func *function, void *aux)

{

    ASSERT (function != NULL);



    intr_enable (); /* The scheduler runs with interrupts off. */

    function (aux); /* Execute the thread function. */

    thread_exit (); /* If function() returns, kill the thread. */

}



/* Returns the running thread. */

struct thread *

running_thread (void)

{

    uint32_t *esp;



    /* Copy the CPU's stack pointer into `esp', and then round that

     down to the start of a page.  Because `struct thread' is

     always at the beginning of a page and the stack pointer is

     somewhere in the middle, this locates the curent thread. */

    asm ("mov %%esp, %0" : "=g"(esp));

    return pg_round_down (esp);

}



/* Returns true if T appears to point to a valid thread. */

static bool

is_thread (struct thread *t)

{

    return t != NULL && t->magic == THREAD_MAGIC;

}



/* Does basic initialization of T as a blocked thread named

   NAME. */



static void

init_thread (struct thread *t, const char *name, int priority)

{

    ASSERT (t != NULL);

    ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);

    ASSERT (name != NULL);



    memset (t, 0, sizeof *t);

    t->status = THREAD_BLOCKED;

    strlcpy (t->name, name, sizeof t->name);

    t->stack = (uint8_t *)t + PGSIZE;

    

    /* 스레드의 기본 필드 설정 */

    t->priority = priority;

    t->age = 0; 



    /* MLFQS 필드 초기화 */

    if (thread_mlfqs)

    {

        t->mlfqs_level = 0;    /* 모든 스레드는 Q0에서 시작 */

        t->ticks_in_slice = 0;

    }



    t->magic = THREAD_MAGIC;

    

    /* all_list에는 딱 한 번만 추가합니다. */

    list_push_back (&all_list, &t->allelem);

}





/* Allocates a SIZE-byte frame at the top of thread T's stack and

   returns a pointer to the frame's base. */

static void *

alloc_frame (struct thread *t, size_t size)

{

    /* Stack data is always allocated in word-size units. */

    ASSERT (is_thread (t));

    ASSERT (size % sizeof (uint32_t) == 0);



    t->stack -= size;

    return t->stack;

}



/* Chooses and returns the next thread to be scheduled.  Should

   return a thread from the run queue, unless the run queue is

   empty.  (If the running thread can continue running, then it

   will be in the run queue.)  If the run queue is empty, return

   idle_thread. */

static struct thread *

next_thread_to_run (void)

{

    if (thread_mlfqs)

    {

        /* [FIX 7-1] MLFQS: Q0, Q1, Q2 순서대로 큐가 비어있는지 확인 */

        if (!list_empty (&mlfqs_ready_queues[0]))

            return list_entry (list_pop_front (&mlfqs_ready_queues[0]), struct thread, elem);

        if (!list_empty (&mlfqs_ready_queues[1]))

            return list_entry (list_pop_front (&mlfqs_ready_queues[1]), struct thread, elem);

        if (!list_empty (&mlfqs_ready_queues[2]))

            return list_entry (list_pop_front (&mlfqs_ready_queues[2]), struct thread, elem);

    }

    else

    {

        /* [FIX 7-2] Priority: 기존 로직 */

        if (!list_empty (&ready_list))

            return list_entry (list_pop_front (&ready_list), struct thread, elem);

    }

    

    return idle_thread;

}



/* Completes a thread switch by activating the new thread's page

   tables, and, if the previous thread is dying, destroying it.



   At this function's invocation, we just switched from thread

   PREV, the new thread is already running, and interrupts are

   still disabled.  This function is normally invoked by

   thread_schedule() as its final action before returning, but

   the first time a thread is scheduled it is called by

   switch_entry() (see switch.S).



   It's not safe to call printf() until the thread switch is

   complete.  In practice that means that printf()s should be

   added at the end of the function.



   After this function and its caller returns, the thread switch

   is complete. */

void

thread_schedule_tail (struct thread *prev)

{

    struct thread *cur = running_thread ();



    ASSERT (intr_get_level () == INTR_OFF);



    /* Mark us as running. */

    cur->status = THREAD_RUNNING;



    /* Start new time slice. */

    thread_ticks = 0;



#ifdef USERPROG

    /* Activate the new address space. */

    process_activate ();

#endif



    /* If the thread we switched from is dying, destroy its struct

     thread.  This must happen late so that thread_exit() doesn't

     pull out the rug under itself.  (We don't free

     initial_thread because its memory was not obtained via

     palloc().) */

    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)

        {

            ASSERT (prev != cur);

            palloc_free_page (prev);

        }

}



/* Schedules a new process.  At entry, interrupts must be off and

   the running process's state must have been changed from

   running to some other state.  This function finds another

   thread to run and switches to it.



   It's not safe to call printf() until thread_schedule_tail()

   has completed. */

static void

schedule (void)

{

    struct thread *cur = running_thread ();

    struct thread *next = next_thread_to_run ();

    struct thread *prev = NULL;



    ASSERT (intr_get_level () == INTR_OFF);

    ASSERT (cur->status != THREAD_RUNNING);

    ASSERT (is_thread (next));



    if (cur != next)

        prev = switch_threads (cur, next);

    thread_schedule_tail (prev);

}



/* Returns a tid to use for a new thread. */

static tid_t

allocate_tid (void)

{

    static tid_t next_tid = 1;

    tid_t tid;



    lock_acquire (&tid_lock);

    tid = next_tid++;

    lock_release (&tid_lock);



    return tid;

}

uint32_t thread_stack_ofs = offsetof (struct thread, stack);
