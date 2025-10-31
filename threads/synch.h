#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

struct semaphore
{
    unsigned value;
    struct list waiters;
};

struct semaphore_elem
{
    struct list_elem elem;
    struct semaphore semaphore;
    int priority;
};

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

struct lock
{
    struct thread *holder;
    struct semaphore semaphore;
};

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

struct condition
{
    struct list waiters;
};

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

#define barrier() asm volatile ("" : : : "memory")

bool sema_elem_priority_less_func (const struct list_elem *a,
                                   const struct list_elem *b,
                                   void *aux);
#endif
