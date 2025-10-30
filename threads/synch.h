
#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* 세마포어. */
struct semaphore {
  unsigned value;             /* Current value. */
  struct list waiters;        /* Priority-ordered list of waiting threads. */
};

/* 락. */
struct lock {
  struct thread *holder;      /* Thread holding lock (for debugging). */
  struct semaphore semaphore; /* Binary semaphore controlling access. */
};

/* 조건 변수. */
struct condition {
  struct list waiters;        /* List of semaphore_elem, priority-ordered. */
};

/* 초기화 */
void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

/* 락 */
void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* 조건 변수 */
void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/* 내부 비교자 */
bool sema_waiter_priority_higher (const struct list_elem *, const struct list_elem *, void *);
bool cond_waiter_priority_higher (const struct list_elem *, const struct list_elem *, void *);

#endif /* threads/synch.h */
