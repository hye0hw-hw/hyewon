#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

void
sema_init (struct semaphore *sema, unsigned value)
{
    ASSERT (sema != NULL);

    sema->value = value;
    list_init (&sema->waiters);
}

void
sema_down (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    ASSERT (!intr_context ());

    old_level = intr_disable ();
    while (sema->value == 0)
    {
        if (thread_mlfqs)
        {
            list_push_back (&sema->waiters, &thread_current ()->elem);
        }
        else
        {
            list_insert_ordered (&sema->waiters, &thread_current ()->elem,
                                 thread_priority_less_func, NULL);
        }
        thread_block ();
    }
    sema->value--;
    intr_set_level (old_level);
}

bool
sema_try_down (struct semaphore *sema)
{
    enum intr_level old_level;
    bool success;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (sema->value > 0)
        {
            sema->value--;
            success = true;
        }
    else
        success = false;
    intr_set_level (old_level);

    return success;
}

void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;
    struct thread *t = NULL;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
    if (!list_empty (&sema->waiters))
    {
        t = list_entry (list_pop_front (&sema->waiters),
                        struct thread, elem);
        thread_unblock (t);
    }
    sema->value++;

    if (t != NULL)
    {
        bool preempt = false;
        if (thread_mlfqs) {
            preempt = (t->mlfqs_level < thread_current()->mlfqs_level);
        } else {
            preempt = (t->priority > thread_current()->priority);
        }

        if (preempt && !intr_context()) {
             thread_yield();
        } else if (preempt && intr_context()) {
             intr_yield_on_return();
        }
    }
    
    intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

void
sema_self_test (void)
{
    struct semaphore sema[2];
    int i;

    printf ("Testing semaphores...");
    sema_init (&sema[0], 0);
    sema_init (&sema[1], 0);
    thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
    for (i = 0; i < 10; i++)
        {
            sema_up (&sema[0]);
            sema_down (&sema[1]);
        }
    printf ("done.\n");
}

static void
sema_test_helper (void *sema_)
{
    struct semaphore *sema = sema_;
    int i;

    for (i = 0; i < 10; i++)
        {
            sema_down (&sema[0]);
            sema_up (&sema[1]);
        }
}

void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);

    lock->holder = NULL;
    sema_init (&lock->semaphore, 1);
}

void
lock_acquire (struct lock *lock)
{
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    sema_down (&lock->semaphore);
    lock->holder = thread_current ();
}

bool
lock_try_acquire (struct lock *lock)
{
    bool success;

    ASSERT (lock != NULL);
    ASSERT (!lock_held_by_current_thread (lock));

    success = sema_try_down (&lock->semaphore);
    if (success)
        lock->holder = thread_current ();
    return success;
}

void
lock_release (struct lock *lock)
{
    ASSERT (lock != NULL);
    ASSERT (lock_held_by_current_thread (lock));

    lock->holder = NULL;
    sema_up (&lock->semaphore);
}

bool
lock_held_by_current_thread (const struct lock *lock)
{
    ASSERT (lock != NULL);

    return lock->holder == thread_current ();
}

void
cond_init (struct condition *cond)
{
    ASSERT (cond != NULL);

    list_init (&cond->waiters);
}

bool
sema_elem_priority_less_func (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux UNUSED)
{
    struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
    struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);
    return sa->priority > sb->priority;
}

void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);
    
    waiter.priority = thread_current ()->priority;
    
    if (thread_mlfqs)
    {
        list_push_back (&cond->waiters, &waiter.elem);
    }
    else
    {
        list_insert_ordered (&cond->waiters, &waiter.elem,
                             sema_elem_priority_less_func, NULL);
    }
    
    lock_release (lock);
    sema_down (&waiter.semaphore);
    lock_acquire (lock);
}

void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters))
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                      ->semaphore);
}

void
cond_broadcast (struct condition *cond, struct lock *lock)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);

    while (!list_empty (&cond->waiters))
        cond_signal (cond, lock);
}
