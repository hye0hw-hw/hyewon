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
    void *eip;
    thread_func *function;
    void *aux;
};

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

#define TIME_SLICE 4
static unsigned thread_ticks;

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
                    s_t->mlfqs_level--;
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
                   
                    r_t->age = 0;
                    r_t->mlfqs_level--;
                    
                    r_t->ticks_in_slice = 0; 
                    
                    list_remove(&r_t->elem);
                    list_push_back(&mlfqs_ready_queues[r_t->mlfqs_level], &r_t->elem);
                    
                    e = next_e;
                }
                else
                {
                    e = list_next (e);
                }
            }
        }
        
      
        int slice_limit = 0;
        if (t->mlfqs_level == 0) slice_limit = 2;
        else if (t->mlfqs_level == 1) slice_limit = 4;
        else if (t->mlfqs_level == 2) slice_limit = 8;

        if (t != idle_thread && t->ticks_in_slice >= slice_limit)
        {
            
            t->ticks_in_slice = 0;
            t->age = 0;
            if (t->mlfqs_level < 2)
                t->mlfqs_level++;
            
            intr_yield_on_return();
        }

       
        if (t != idle_thread) 
        {
            bool yield_for_preempt = false;
            if (t->mlfqs_level == 1 && !list_empty(&mlfqs_ready_queues[0]))
            {
                yield_for_preempt = true;
            }
            else if (t->mlfqs_level == 2)
            {
                if (!list_empty(&mlfqs_ready_queues[0]) || !list_empty(&mlfqs_ready_queues[1]))
                   yield_for_preempt = true;
            }
            
            if (yield_for_preempt) {
                 intr_yield_on_return();
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

        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return ();
            
    } 
}

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

struct thread *
thread_current (void)
{
    struct thread *t = running_thread ();

    ASSERT (is_thread (t));
    ASSERT (t->status == THREAD_RUNNING);

    return t;
}

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
            list_push_back (&mlfqs_ready_queues[cur->mlfqs_level], &cur->elem);
        }
        else
        {
            list_insert_ordered (&ready_list, &cur->elem, thread_priority_less_func, NULL);
        }
    }
    cur->status = THREAD_READY;
    schedule ();
    intr_set_level (old_level);
}

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

void
thread_set_priority (int new_priority)
{
    thread_current ()->priority = new_priority;

    if (!thread_mlfqs && !list_empty (&ready_list))
    {
        struct thread *highest_ready = 
            list_entry (list_front (&ready_list), struct thread, elem);

        if (new_priority < highest_ready->priority)
        {
            thread_yield ();
        }
    }
}

int
thread_get_priority (void)
{
    if (thread_mlfqs)
    {
        struct thread *cur = thread_current();
        if (cur->mlfqs_level == 0) {
            return PRI_MAX;
        } else if (cur->mlfqs_level == 1) {
            return PRI_DEFAULT;
        } else {
            return PRI_MIN;
        }
    }
    else
    {
       return thread_current ()->priority;
    }
}

void
thread_set_nice (int nice UNUSED)
{
}

int
thread_get_nice (void)
{
    return 0;
}

int
thread_get_load_avg (void)
{
    return 0;
}

int
thread_get_recent_cpu (void)
{
    return 0;
}

static void
idle (void *idle_started_ UNUSED)
{
    struct semaphore *idle_started = idle_started_;
    idle_thread = thread_current ();
    sema_up (idle_started);

    for (;;)
        {
            intr_disable ();
            thread_block ();

            asm volatile ("sti; hlt" : : : "memory");
        }
}

static void
kernel_thread (thread_func *function, void *aux)
{
    ASSERT (function != NULL);

    intr_enable ();
    function (aux);
    thread_exit ();
}

struct thread *
running_thread (void)
{
    uint32_t *esp;

    asm ("mov %%esp, %0" : "=g"(esp));
    return pg_round_down (esp);
}

static bool
is_thread (struct thread *t)
{
    return t != NULL && t->magic == THREAD_MAGIC;
}

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
    
    t->priority = priority;
    t->age = 0; 

    if (thread_mlfqs)
    {
        t->mlfqs_level = 0;
        t->ticks_in_slice = 0;
    }

    t->magic = THREAD_MAGIC;
    
    list_push_back (&all_list, &t->allelem);
}

static void *
alloc_frame (struct thread *t, size_t size)
{
    ASSERT (is_thread (t));
    ASSERT (size % sizeof (uint32_t) == 0);

    t->stack -= size;
    return t->stack;
}

static struct thread *
next_thread_to_run (void)
{
    if (thread_mlfqs)
    {
        if (!list_empty (&mlfqs_ready_queues[0]))
            return list_entry (list_pop_front (&mlfqs_ready_queues[0]), struct thread, elem);
        if (!list_empty (&mlfqs_ready_queues[1]))
            return list_entry (list_pop_front (&mlfqs_ready_queues[1]), struct thread, elem);
        if (!list_empty (&mlfqs_ready_queues[2]))
            return list_entry (list_pop_front (&mlfqs_ready_queues[2]), struct thread, elem);
    }
    else
    {
        if (!list_empty (&ready_list))
            return list_entry (list_pop_front (&ready_list), struct thread, elem);
    }
    
    return idle_thread;
}

void
thread_schedule_tail (struct thread *prev)
{
    struct thread *cur = running_thread ();

    ASSERT (intr_get_level () == INTR_OFF);

    cur->status = THREAD_RUNNING;

    thread_ticks = 0;

#ifdef USERPROG
    process_activate ();
#endif

    if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
        {
            ASSERT (prev != cur);
            palloc_free_page (prev);
        }
}

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
