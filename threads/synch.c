

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"

static bool cond_sema_priority_more (const struct list_elem *a,
                                     const struct list_elem *b,
                                     void *aux UNUSED);


bool
compare_lock_priority (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED)
{
  struct thread *ta = list_entry (a, struct thread, elem);
  struct thread *tb = list_entry (b, struct thread, elem);

  return ta->priority > tb->priority;
}


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
         
            list_insert_ordered (&sema->waiters, &thread_current ()->elem, 
                                 compare_lock_priority, NULL);
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

    ASSERT (sema != NULL);

    old_level = intr_disable ();
if (!list_empty (&sema->waiters)) {
    list_sort (&sema->waiters, compare_lock_priority, NULL);
    struct thread *t = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
    thread_unblock (t);
}

    sema->value++;
    intr_set_level (old_level);
    

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


void
lock_init (struct lock *lock)
{
    ASSERT (lock != NULL);

    lock->holder = NULL;
    // lock->semaphore는 더 이상 사용되지 않습니다.
    list_init (&lock->waiters); // ⭐️ waiters 리스트 초기화 ⭐️
}


void
lock_acquire (struct lock *lock)
{
    enum intr_level old_level;

    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (!lock_held_by_current_thread (lock));

    old_level = intr_disable ();
   
    if (lock->holder != NULL) {
        
        thread_current()->wait_on_lock = lock;
        list_insert_ordered (&lock->waiters, &thread_current ()->elem, 
                             compare_lock_priority, NULL);
        list_insert_ordered (&lock->holder->donations, &thread_current ()->donation_elem,
                             thread_compare_donation_priority, NULL);
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


void
lock_release (struct lock *lock)
{
    enum intr_level old_level;
    
    ASSERT (lock != NULL);
    ASSERT (lock_held_by_current_thread (lock));

    old_level = intr_disable ();
    

    thread_remove_donations(lock);
    thread_update_priority(thread_current()); // 우선순위 복구/갱신

   
    lock->holder = NULL;
    if (!list_empty (&lock->waiters))
       
        thread_unblock (list_entry (list_pop_front (&lock->waiters),
                                    struct thread, elem));
                                    
    intr_set_level (old_level);
    
  
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


void
cond_wait (struct condition *cond, struct lock *lock)
{
    struct semaphore_elem waiter;

    ASSERT (cond != NULL);
    ASSERT (lock != NULL);
    ASSERT (!intr_context ());
    ASSERT (lock_held_by_current_thread (lock));

    sema_init (&waiter.semaphore, 0);

    list_push_back (&cond->waiters, &waiter.elem);
    
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
  {

    list_sort (&cond->waiters, cond_sema_priority_more, NULL);

    struct semaphore_elem *se = list_entry (
        list_pop_front (&cond->waiters),
        struct semaphore_elem,
        elem);
    sema_up (&se->semaphore);
  }
}



void
cond_broadcast (struct condition *cond, struct lock *lock)
{
    ASSERT (cond != NULL);
    ASSERT (lock != NULL);

    while (!list_empty (&cond->waiters))
        cond_signal (cond, lock);
}

static bool
cond_sema_priority_more (const struct list_elem *a,
                         const struct list_elem *b,
                         void *aux UNUSED)
{
 struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
 struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

  struct thread *ta = list_entry (list_front (&sa->semaphore.waiters), struct thread, elem);
  struct thread *tb = list_entry (list_front (&sb->semaphore.waiters), struct thread, elem);

  return ta->priority > tb->priority;
}
