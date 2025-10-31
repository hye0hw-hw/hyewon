#include "threads/synch.h"

#include <stdio.h>

#include <string.h>

#include "threads/interrupt.h"

#include "threads/thread.h"



/* Initializes semaphore SEMA to VALUE.  A semaphore is a

   nonnegative integer along with two atomic operators for

   manipulating it:



   - down or "P": wait for the value to become positive, then

     decrement it.



   - up or "V": increment the value (and wake up one waiting

     thread, if any). */

void

sema_init (struct semaphore *sema, unsigned value)

{

    ASSERT (sema != NULL);



    sema->value = value;

    list_init (&sema->waiters);

}



/* Down or "P" operation on a semaphore.  Waits for SEMA's value

   to become positive and then atomically decrements it.



   This function may sleep, so it must not be called within an

   interrupt handler.  This function may be called with

   interrupts disabled, but if it sleeps then the next scheduled

   thread will probably turn interrupts back on. */

/* synch.c */
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
            /* [FIX] MLFQS: FIFO 순서로 대기열 끝에 추가 */
            list_push_back (&sema->waiters, &thread_current ()->elem);
        }
        else
        {
            /* Priority: 기존 우선순위 정렬 */
            list_insert_ordered (&sema->waiters, &thread_current ()->elem,
                                 thread_priority_less_func, NULL);
        }
        thread_block ();
    }
    sema->value--;
    intr_set_level (old_level);
}


/* Down or "P" operation on a semaphore, but only if the

   semaphore is not already 0.  Returns true if the semaphore is

   decremented, false otherwise.



   This function may be called from an interrupt handler. */

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



/* Up or "V" operation on a semaphore.  Increments SEMA's value

   and wakes up one thread of those waiting for SEMA, if any.



   This function may be called from an interrupt handler. */

   

/* synch.c - function sema_up() */

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



    /* [FIX 10] MLFQS와 Priority 스케줄링 모두 지원하도록 선점 로직 수정 */

    if (t != NULL)

    {

        bool preempt = false;

        if (thread_mlfqs) {

            /* MLFQS: 큐 레벨(숫자가 낮은)이 더 높으면 선점 */

            preempt = (t->mlfqs_level < thread_current()->mlfqs_level);

        } else {

            /* Priority: 우선순위(숫자가 큰)가 더 높으면 선점 */

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



/* Self-test for semaphores that makes control "ping-pong"

   between a pair of threads.  Insert calls to printf() to see

   what's going on. */

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



/* Thread function used by sema_self_test(). */

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



/* Initializes LOCK.  A lock can be held by at most a single

   thread at any given time.  Our locks are not "recursive", that

   is, it is an error for the thread currently holding a lock to

   try to acquire that lock.



   A lock is a specialization of a semaphore with an initial

   value of 1.  The difference between a lock and such a

   semaphore is twofold.  First, a semaphore can have a value

   greater than 1, but a lock can only be owned by a single

   thread at a time.  Second, a semaphore does not have an owner,

   meaning that one thread can "down" the semaphore and then

   another one "up" it, but with a lock the same thread must both

   acquire and release it.  When these restrictions prove

   onerous, it's a good sign that a semaphore should be used,

   instead of a lock. */

void

lock_init (struct lock *lock)

{

    ASSERT (lock != NULL);



    lock->holder = NULL;

    sema_init (&lock->semaphore, 1);

}



/* Acquires LOCK, sleeping until it becomes available if

   necessary.  The lock must not already be held by the current

   thread.



   This function may sleep, so it must not be called within an

   interrupt handler.  This function may be called with

   interrupts disabled, but interrupts will be turned back on if

   we need to sleep. */

void

lock_acquire (struct lock *lock)

{

    ASSERT (lock != NULL);

    ASSERT (!intr_context ());

    ASSERT (!lock_held_by_current_thread (lock));



    sema_down (&lock->semaphore);

    lock->holder = thread_current ();

}



/* Tries to acquires LOCK and returns true if successful or false

   on failure.  The lock must not already be held by the current

   thread.



   This function will not sleep, so it may be called within an

   interrupt handler. */

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



/* Releases LOCK, which must be owned by the current thread.



   An interrupt handler cannot acquire a lock, so it does not

   make sense to try to release a lock within an interrupt

   handler. */

void

lock_release (struct lock *lock)

{

    ASSERT (lock != NULL);

    ASSERT (lock_held_by_current_thread (lock));



    lock->holder = NULL;

    sema_up (&lock->semaphore);

}



/* Returns true if the current thread holds LOCK, false

   otherwise.  (Note that testing whether some other thread holds

   a lock would be racy.) */

bool

lock_held_by_current_thread (const struct lock *lock)

{

    ASSERT (lock != NULL);



    return lock->holder == thread_current ();

}





/* Initializes condition variable COND.  A condition variable

   allows one piece of code to signal a condition and cooperating

   code to receive the signal and act upon it. */

void

cond_init (struct condition *cond)

{

    ASSERT (cond != NULL);



    list_init (&cond->waiters);

}



/* Atomically releases LOCK and waits for COND to be signaled by

   some other piece of code.  After COND is signaled, LOCK is

   reacquired before returning.  LOCK must be held before calling

   this function.



   The monitor implemented by this function is "Mesa" style, not

   "Hoare" style, that is, sending and receiving a signal are not

   an atomic operation.  Thus, typically the caller must recheck

   the condition after the wait completes and, if necessary, wait

   again.



   A given condition variable is associated with only a single

   lock, but one lock may be associated with any number of

   condition variables.  That is, there is a one-to-many mapping

   from locks to condition variables.



   This function may sleep, so it must not be called within an

   interrupt handler.  This function may be called with

   interrupts disabled, but interrupts will be turned back on if

   we need to sleep. */

/* * [FIX 3] semaphore_elem을 우선순위(내림차순)로 비교하는 함수*/

 

bool

sema_elem_priority_less_func (const struct list_elem *a,

                              const struct list_elem *b,

                              void *aux UNUSED)

{

    struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);

    struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

    return sa->priority > sb->priority; // 높은 우선순위가 앞에 오도록

}

   

   

   

   

/* cond_wait 수정 */

/* synch.c */
void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);
    
    /* waiter.priority는 Priority 스케줄러에서만 사용됩니다. */
    waiter.priority = thread_current ()->priority; 
    
    if (thread_mlfqs)
    {
        /* [FIX] MLFQS: FIFO 순서로 대기열 끝에 추가 */
        list_push_back (&cond->waiters, &waiter.elem);
    }
    else
    {
        /* Priority: 기존 우선순위 정렬 */
        list_insert_ordered (&cond->waiters, &waiter.elem, 
                             sema_elem_priority_less_func, NULL);
    }
    
    lock_release (lock);
    sema_down (&waiter.semaphore);
    lock_acquire (lock);
}


/* If any threads are waiting on COND (protected by LOCK), then

   this function signals one of them to wake up from its wait.

   LOCK must be held before calling this function.



   An interrupt handler cannot acquire a lock, so it does not

   make sense to try to signal a condition variable within an

   interrupt handler. */

/* cond_signal 수정 */

void

cond_signal (struct condition *cond, struct lock *lock UNUSED)

{

    ASSERT (cond != NULL);

    ASSERT (lock != NULL);

    ASSERT (!intr_context ());

    ASSERT (lock_held_by_current_thread (lock));



    /*

     * cond_wait에서 리스트가 정렬되었으므로,

     * list_pop_front는 항상 가장 우선순위가 높은 waiter를 반환합니다.

     * (sema_up은 Fix 1에서 수정했으므로 선점 로직이 동작합니다.)

     */

    if (!list_empty (&cond->waiters))

        sema_up (&list_entry (list_pop_front (&cond->waiters),

                              struct semaphore_elem, elem)

                 ->semaphore);

}



/* Wakes up all threads, if any, waiting on COND (protected by

   LOCK).  LOCK must be held before calling this function.



   An interrupt handler cannot acquire a lock, so it does not

   make sense to try to signal a condition variable within an

   interrupt handler. */

void

cond_broadcast (struct condition *cond, struct lock *lock)

{

    ASSERT (cond != NULL);

    ASSERT (lock != NULL);



    while (!list_empty (&cond->waiters))

        cond_signal (cond, lock);

}



