

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"

/* ⭐️ 락/세마포어 대기열 정렬을 위한 비교 함수 ⭐️
   가장 높은 우선순위가 리스트의 앞(list_front)에 오도록 합니다. */
bool
compare_lock_priority (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED)
{
  struct thread *ta = list_entry (a, struct thread, elem);
  struct thread *tb = list_entry (b, struct thread, elem);

  return ta->priority > tb->priority;
}

/* Initializes semaphore SEMA to VALUE. A semaphore is a
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

/* Down or "P" operation on a semaphore. Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler. This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);
    ASSERT (!intr_context ());

    old_level = intr_disable ();
    while (sema->value == 0)
        {
            // ⭐️ 우선순위 순으로 삽입 ⭐️
            list_insert_ordered (&sema->waiters, &thread_current ()->elem, 
                                 compare_lock_priority, NULL);
            thread_block ();
        }
    sema->value--;
    intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0. Returns true if the semaphore is
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

/* Up or "V" operation on a semaphore. Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema)
{
    enum intr_level old_level;

    ASSERT (sema != NULL);

    old_level = intr_disable ();
if (!list_empty (&sema->waiters)) {
    list_sort (&sema->waiters, compare_lock_priority, NULL);
    struct thread *t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
    thread_unblock (t);
}

    sema->value++;
    intr_set_level (old_level);
    
    // ⭐️ sema_up 후 선점 검사 ⭐️
    thread_yield();
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads. Insert calls to printf() to see
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

/* Initializes LOCK. A lock can be held by at most a single
   thread at any given time. Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1. The difference between a lock and such a
   semaphore is twofold. First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time. Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it. When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);

    lock->holder = NULL;
    // lock->semaphore는 더 이상 사용되지 않습니다.
    list_init (&lock->waiters); // ⭐️ waiters 리스트 초기화 ⭐️
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary. The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler. This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
    enum intr_level old_level;

    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    old_level = intr_disable ();
    
    // 락 홀더가 있다면 (획득 실패)
    if (lock->holder != NULL) {
        
        // 1. 대기 스레드 정보 설정 및 리스트 삽입
        thread_current()->wait_on_lock = lock;
        list_insert_ordered (&lock->waiters, &thread_current ()->elem, 
                             compare_lock_priority, NULL);
        list_insert_ordered (&lock->holder->donations, &thread_current ()->donation_elem,
                             thread_compare_donation_priority, NULL);

        // 2. ⭐️ 연쇄 기부 로직 ⭐️
        struct thread *t = lock->holder;
        while (t != NULL && t->wait_on_lock != NULL) {
            thread_update_priority(t);
            t = t->wait_on_lock->holder; 
        }
        
        // 3. 현재 락 홀더의 우선순위를 최종 갱신
        thread_update_priority(lock->holder);
        
        thread_block ();
        
        // 4. block에서 깨어난 후: 락을 얻었으므로 wait_on_lock을 NULL로 초기화
        thread_current()->wait_on_lock = NULL; 
    }
    
    // 락 획득
    lock->holder = thread_current ();
    intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure. The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
    bool success;
    enum intr_level old_level;

    ASSERT (lock != NULL);
    ASSERT (!lock_held_by_current_thread (lock));
    
    old_level = intr_disable ();
    
    if (lock->holder == NULL) {
        lock->holder = thread_current ();
        success = true;
    } else {
        success = false;
    }
    
    intr_set_level (old_level);
    return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock)
{
    enum intr_level old_level;
    
    ASSERT (lock != NULL);
    ASSERT (lock_held_by_current_thread (lock));

    old_level = intr_disable ();
    
    // ⭐️ 기부 철회 로직 ⭐️
    thread_remove_donations(lock);
    thread_update_priority(thread_current()); // 우선순위 복구/갱신

    // 락 홀더 해제 및 대기 스레드 깨우기
    lock->holder = NULL;
    if (!list_empty (&lock->waiters))
        // ⭐️ 가장 높은 우선순위 스레드 unblock ⭐️
        thread_unblock (list_entry (list_pop_front (&lock->waiters),
                                    struct thread, elem));
                                    
    intr_set_level (old_level);
    
    // ⭐️ 선점 검사 ⭐️
    thread_yield();
}

/* Returns true if the current thread holds LOCK, false
   otherwise. (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock)
{
    ASSERT (lock != NULL);

    return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem
{
    struct list_elem elem;          /* List element. */
    struct semaphore semaphore; /* This semaphore. */
};

void
cond_init (struct condition *cond)
{
    ASSERT (cond != NULL);

    list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code. After COND is signaled, LOCK is
   reacquired before returning. LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation. Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables. That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler. This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);

    list_insert_ordered (&cond->waiters, &waiter.elem, compare_lock_priority, NULL);
    
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
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    if (!list_empty (&cond->waiters))
        // ⭐️ 가장 높은 우선순위 스레드에게 signal ⭐️
        sema_up (&list_entry (list_pop_front (&cond->waiters),
                              struct semaphore_elem, elem)
                         ->semaphore);
   if (!list_empty (&cond->waiters)) {
    list_sort (&cond->waiters, compare_lock_priority, NULL);
    sema_up (&list_entry (list_pop_front (&cond->waiters),
                          struct semaphore_elem, elem)->semaphore);
}

}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK). LOCK must be held before calling this function.

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
