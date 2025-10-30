#include "threads/thread.h"
#include <debug.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "devices/timer.h"

/* --- switch.S에서 참조하는 전역 변수 --- */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

/* 스케줄러용 큐 */
static struct list ready_list;
static struct list q0_list, q1_list, q2_list;
 struct list sleep_list;
/* current_ticks 시각까지 도달한 잠자는 스레드를 깨운다. */

/* 스레드 우선순위 비교 함수 (ready_list 정렬용) */
bool
thread_compare_priority(const struct list_elem *a,
                        const struct list_elem *b,
                        void *aux UNUSED)
{
  const struct thread *ta = list_entry(a, struct thread, elem);
  const struct thread *tb = list_entry(b, struct thread, elem);
  return ta->priority > tb->priority;
}

void
thread_wake (int64_t current_ticks)
{
  while (!list_empty (&sleep_list)) {
    struct thread *t = list_entry (list_front (&sleep_list), struct thread, elem);
    if (t->wake_tick <= current_ticks) {
      list_pop_front (&sleep_list);
      thread_unblock (t);              /* -> READY로 */
    } else {
      break;                           /* 리스트가 정렬되어 있으므로 이후는 전부 아직 */
    }
  }
}

/* 깨울 시각 오름차순 정렬 함수 (list_insert_ordered에 사용) */
 bool wake_tick_less (const struct list_elem *a,
                            const struct list_elem *b,
                            void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->wake_tick < tb->wake_tick;
}

static struct thread *idle_thread;
static struct thread *initial_thread;
static bool thread_started;

static struct lock tid_lock;
static tid_t next_tid = 1;

/* mlfqs 플래그 */
bool thread_mlfqs = false;

/* 내부 변수 */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

/* 내부 함수 원형 */
static void idle (void *aux UNUSED);
static void init_thread (struct thread *, const char *name, int priority);
static struct thread *running_thread (void);
static tid_t allocate_tid (void);
static void schedule (void);
static struct thread *next_thread_to_run (void);
static void ready_push (struct thread *t);
static struct thread *ready_pop (void);
static void preempt_if_needed (void);
static void aging_on_tick (void);
static void aging_on_enqueue (struct thread *t);
static void mlfqs_tick_update_running (struct thread *cur);
static void mlfqs_maybe_preempt_on_unblock (struct thread *unblocked);
static void promote_priority_one (struct thread *t);
static void mlfqs_promote_queue_one (struct thread *t);

/* 매직 넘버 */
#define THREAD_MAGIC 0xcd6abf4b

/* --- 공개 함수 --- */

void
thread_set_mlfqs_enabled (bool on) {
  thread_mlfqs = on;
}

bool
thread_mlfqs_enabled (void) {
  return thread_mlfqs;
}

bool
thread_is_mlfqs (void) {
  return thread_mlfqs;
}

int
thread_effective_priority (const struct thread *t) {
  return t ? t->priority : PRI_MIN;
}

bool
thread_priority_higher (const struct list_elem *a,
                        const struct list_elem *b, void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  int pa = thread_effective_priority (ta);
  int pb = thread_effective_priority (tb);
  return pa > pb;
}

/* --- 초기화 --- */

void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&q0_list);
  list_init (&q1_list);
  list_init (&q2_list);
   list_init (&sleep_list);

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  thread_started = false;
}

void
thread_start (void)
{
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  intr_enable ();
  sema_down (&idle_started);
  thread_started = true;
}

/* 매 틱마다 호출 */
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

  aging_on_tick ();
  preempt_if_needed ();
}

void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* --- 스레드 생성/관리 --- */

tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux UNUSED)
{
  struct thread *t;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  enum intr_level old_level = intr_disable ();
  thread_unblock (t);
  intr_set_level (old_level);

  if (thread_effective_priority (t) > thread_effective_priority (thread_current ()))
    thread_yield ();

  return tid;
}

struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();
  ASSERT (is_thread (t));
  return t;
}

const char *
thread_name (void)
{
  return thread_current ()->name;
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
  list_remove (&thread_current ()->elem);
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
  if (cur != idle_thread) {
    cur->status = THREAD_READY;
    ready_push (cur);
  }
  schedule ();
  intr_set_level (old_level);
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
list_insert_ordered (&ready_list, &t->elem, thread_compare_priority, NULL);
  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  t->status = THREAD_READY;
  aging_on_enqueue (t);
  ready_push (t);
  intr_set_level (old_level);

  if (thread_mlfqs)
    mlfqs_maybe_preempt_on_unblock (t);
  else if (thread_effective_priority (t) > thread_effective_priority (thread_current ())) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

/* --- 우선순위 --- */
int
thread_get_priority (void)
{
  return thread_effective_priority (thread_current ());
}

void
thread_set_priority (int new_priority)
{
  struct thread *cur = thread_current ();
  if (thread_mlfqs) return;

  cur->base_priority = new_priority;
  cur->priority = new_priority;
  preempt_if_needed ();
}

/* --- 내부 함수 --- */

static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) ((uint8_t *) t + PGSIZE);
  t->priority = priority;
  t->base_priority = priority;
  t->age = 0;
  t->mlfqs = thread_mlfqs;
  t->mlfqs_level = 0;
  t->time_slice_used = 0;
  t->magic = THREAD_MAGIC;
}

static struct thread *
running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((uintptr_t) esp & ~ (PGSIZE - 1));
}

static void
idle (void *idle_started_)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) {
    intr_disable ();
    thread_block ();
    asm volatile ("sti; hlt" : : : "memory");
  }
}

/* ready 큐 관리 */
static void
ready_push (struct thread *t)
{
  if (!t) return;
  if (thread_mlfqs) {
    if (t->mlfqs_level <= 0) list_push_back (&q0_list, &t->elem);
    else if (t->mlfqs_level == 1) list_push_back (&q1_list, &t->elem);
    else list_push_back (&q2_list, &t->elem);
  } else {
    list_insert_ordered (&ready_list, &t->elem, thread_priority_higher, NULL);
  }
}

static struct thread *
ready_pop (void)
{
  if (thread_mlfqs) {
    struct list *sel = NULL;
    if (!list_empty (&q0_list)) sel = &q0_list;
    else if (!list_empty (&q1_list)) sel = &q1_list;
    else if (!list_empty (&q2_list)) sel = &q2_list;
    if (!sel) return NULL;
    struct list_elem *e = list_pop_front (sel);
    return list_entry (e, struct thread, elem);
  } else {
    if (list_empty (&ready_list)) return NULL;
    struct list_elem *e = list_pop_front (&ready_list);
    return list_entry (e, struct thread, elem);
  }
}

static struct thread *
next_thread_to_run (void)
{
  struct thread *t = ready_pop ();
  return t ? t : idle_thread;
}

/* 스케줄 */
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

thread_schedule_tail (prev);  // ★ 항상 호출되어야 status가 RUNNING으로 복구됨

}

void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();   // ★ 현재 실행 중 스레드 가져오기
  cur->status = THREAD_RUNNING;             // 실행 중 상태로 변경

  /* 이전 스레드가 종료(DYING) 상태면 스택 해제 */
  if (prev != NULL && prev->status == THREAD_DYING && prev != cur)
    palloc_free_page (prev);
}


static tid_t
allocate_tid (void)
{
  lock_acquire (&tid_lock);
  tid_t tid = next_tid++;
  lock_release (&tid_lock);
  return tid;
}

/* --- 에이징 및 MLFQS --- */
static void
aging_on_enqueue (struct thread *t)
{
  if (t) t->age = 0;
}

static void
promote_priority_one (struct thread *t)
{
  if (t && t->priority < PRI_MAX)
    t->priority++;
}

static void
mlfqs_promote_queue_one (struct thread *t)
{
  if (t && t->mlfqs_level > 0)
    t->mlfqs_level--;
}

static void
aging_on_tick (void)
{
  if (thread_mlfqs) {
    struct list *queues[3] = { &q0_list, &q1_list, &q2_list };
    for (int q = 0; q < 3; q++) {
      for (struct list_elem *e = list_begin (queues[q]); e != list_end (queues[q]);) {
        struct thread *t = list_entry (e, struct thread, elem);
        e = list_next (e);
        t->age++;
        if (t->age >= 20) {
          t->age = 0;
          mlfqs_promote_queue_one (t);
          list_remove (&t->elem);
          ready_push (t);
        }
      }
    }
  } else {
    for (struct list_elem *e = list_begin (&ready_list); e != list_end (&ready_list);) {
      struct thread *t = list_entry (e, struct thread, elem);
      e = list_next (e);
      t->age++;
      if (t->age >= 20) {
        t->age = 0;
        promote_priority_one (t);
        list_remove (&t->elem);
        list_insert_ordered (&ready_list, &t->elem, thread_priority_higher, NULL);
      }
    }
  }
}

static void
mlfqs_tick_update_running (struct thread *cur)
{
  if (cur == idle_thread) return;
  cur->time_slice_used++;
  int limit = (cur->mlfqs_level == 0) ? 2 : (cur->mlfqs_level == 1 ? 4 : 8);
  if (cur->mlfqs_level > 0 && !list_empty (&q0_list)) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
    return;
  }
  if (cur->time_slice_used >= limit) {
    if (cur->mlfqs_level < 2) cur->mlfqs_level++;
    cur->time_slice_used = 0;
    enum intr_level old = intr_disable ();
    cur->status = THREAD_READY;
    ready_push (cur);
    schedule ();
    intr_set_level (old);
  }
}

static void
mlfqs_maybe_preempt_on_unblock (struct thread *unblocked)
{
  struct thread *cur = thread_current ();
  if (cur == idle_thread) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
    return;
  }
  int c = cur->mlfqs_level, u = unblocked->mlfqs_level;
  bool pre = (c == 1 && u == 0) || (c == 2 && (u == 0 || u == 1));
  if (pre) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

static void
preempt_if_needed (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old = intr_disable ();

  int cur_p = thread_effective_priority (cur);
  int top_p = cur_p;

  if (thread_mlfqs) {
    if (!list_empty (&q0_list)) top_p = PRI_MAX;
    else if (!list_empty (&q1_list) && cur->mlfqs_level > 1) top_p = PRI_MAX - 1;
  } else {
    if (!list_empty (&ready_list)) {
      struct thread *front = list_entry (list_front (&ready_list), struct thread, elem);
      int ready_p = thread_effective_priority (front);
      if (ready_p > top_p) top_p = ready_p;
    }
  }

  intr_set_level (old);
  if (top_p > cur_p) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

/* --- 디버그용 --- */
bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* --- 디버그에서 호출되는 전체 순회 함수 --- */
void
thread_foreach (void (*func)(struct thread *t, void *aux), void *aux UNUSED)
{
  ASSERT (func != NULL);
  func (thread_current (), aux);
}
