#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Priority 비교자: thread.h의 효과적 우선순위 사용 */
bool
sema_waiter_priority_higher (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  int pa = thread_effective_priority (ta);
  int pb = thread_effective_priority (tb);
  if (pa != pb) return pa > pb;
  return false;
}

/* cond_wait용: waiter는 내부에 세마포어와 그 세마포어의 waiters가 있음.
   각 waiter의 대표 우선순위를 비교 */
struct semaphore_elem {
  struct list_elem elem;      /* condition waiters 리스트용 */
  struct semaphore semaphore; /* 이 waiter만을 위한 세마포어 */
  int pri;                    /* 깨어날 스레드의 우선순위를 캐시해 정렬에 사용 */
};

bool
cond_waiter_priority_higher (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED)
{
  const struct semaphore_elem *wa = list_entry (a, struct semaphore_elem, elem);
  const struct semaphore_elem *wb = list_entry (b, struct semaphore_elem, elem);
  if (wa->pri != wb->pri) return wa->pri > wb->pri;
  return false;
}

/* 세마포어 초기화 */
void
sema_init (struct semaphore *sema, unsigned value)
{
  ASSERT (sema != NULL);
  sema->value = value;
  list_init (&sema->waiters);
}

/* P 연산(다운). value가 0이면 우선순위 정렬 대기에 삽입 */
void
sema_down (struct semaphore *sema)
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0)
    {
      /* 우선순위 순으로 waiters에 삽입 */
      list_insert_ordered (&sema->waiters, &thread_current ()->elem,
                           sema_waiter_priority_higher, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* try_down: 바로 획득 시도 */
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

/* V 연산(업). 대기자 중 최고 우선순위를 깨움 */
void
sema_up (struct semaphore *sema)
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters))
    {
      /* 리스트 정렬 보장(대기 중 우선순위 변동 가능성 고려) */
      list_sort (&sema->waiters, sema_waiter_priority_higher, NULL);
      struct thread *t =
          list_entry (list_pop_front (&sema->waiters), struct thread, elem);
      thread_unblock (t);
    }
  sema->value++;
  intr_set_level (old_level);

  /* 더 높은 우선순위가 깨어났다면 선점 */
  if (!intr_context ())
    thread_yield ();
  else
    intr_yield_on_return ();
}

void
sema_self_test (void)
{
}

/* 락 초기화 */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);
  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* 락 획득 */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  sema_down (&lock->semaphore);
  lock->holder = thread_current ();
}

/* try_acquire */
bool
lock_try_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  bool ok = sema_try_down (&lock->semaphore);
  if (ok) lock->holder = thread_current ();
  return ok;
}

/* 락 해제 */
void
lock_release (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

/* 현재 스레드가 해당 락을 보유 중인지 */
bool
lock_held_by_current_thread (const struct lock *lock)
{
  ASSERT (lock != NULL);
  return lock->holder == thread_current ();
}

/* 조건변수 초기화 */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);
  list_init (&cond->waiters);
}

/* cond_wait: 락을 해제하고, signal될 때까지 세마포어로 대기 */
void
cond_wait (struct condition *cond, struct lock *lock)
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  sema_init (&waiter.semaphore, 0);
  waiter.pri = thread_effective_priority (thread_current ());
  /* 대기열을 우선순위 순으로 유지 */
  list_insert_ordered (&cond->waiters, &waiter.elem,
                       cond_waiter_priority_higher, NULL);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* cond_signal: 최고 우선순위 대기자 하나 깨움 */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED)
{
  ASSERT (cond != NULL);
  ASSERT (!intr_context ());
  if (!list_empty (&cond->waiters))
    {
      /* 우선순위 재정렬 */
      list_sort (&cond->waiters, cond_waiter_priority_higher, NULL);
      struct semaphore_elem *best =
          list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem);
      sema_up (&best->semaphore);
    }
}

/* cond_broadcast: 전체 깨움 */
void
cond_broadcast (struct condition *cond, struct lock *lock)
{
  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
