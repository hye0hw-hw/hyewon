#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "devices/timer.h"

/* 내부 함수 원형 */
static void kernel_thread (thread_func *, void *aux);
static void init_thread (struct thread *, const char *name, int priority);
static struct thread *next_thread_to_run (void);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* 전역 객체 */
struct list ready_list;      /* 우선순위 정렬 ready 큐 */
struct list all_list;        /* 생성된 모든 스레드 */
struct list sleep_list;      /* wake_tick 오름차순으로 슬립 보관 */

static struct thread *idle_thread;
static struct thread *initial_thread;

static tid_t next_tid = 1;

bool thread_mlfqs;           /* mlfqs 사용 여부 (기본 false) */

/* 현재 실행 중인 스레드의 스택 포인터가 들어있는 페이지 상단을 thread*로 변환. */
static struct thread *
running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((uintptr_t) esp & ~ (PGSIZE - 1));
}

/* 이 매크로는 커널 스택 오버플로우를 잡아주는 역할. */
#define THREAD_MAGIC 0xcd6abf4b

/* 우선순위 비교: 높은 priority가 앞쪽으로 오게. */
bool
thread_compare_priority (const struct list_elem *a,
                         const struct list_elem *b,
                         void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}

/* 슬립 리스트 정렬: wake_tick 오름차순 */
bool
wake_tick_less (const struct list_elem *a,
                const struct list_elem *b,
                void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  return ta->wake_tick < tb->wake_tick;
}

/* 현재 ticks 기준으로 깨울 애들 깨우기 */
void
thread_wake (int64_t current_ticks)
{
  enum intr_level old = intr_disable ();
  while (!list_empty (&sleep_list))
    {
      struct thread *t =
        list_entry (list_front (&sleep_list), struct thread, elem);
      if (t->wake_tick > current_ticks) break;
      list_pop_front (&sleep_list);
      thread_unblock (t);
    }
  intr_set_level (old);
}

/* 스레드 초기화: 실행할 수 없지만 스케줄링 준비. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);

  /* 초기 스레드(부트 스레드) */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
}

/* idle 스레드 생성하고 스케줄링 시작 준비. */
void
thread_start (void)
{
  /* idle 스레드 생성 */
  tid_t tid = thread_create ("idle", PRI_MIN, &kernel_thread, NULL);
  (void)tid;

  /* 이제 timer 인터럽트가 틱을 발생시키면 스케줄링이 가능. */
}

/* 매 타이머 틱마다 호출됨. */
void
thread_tick (void)
{
  struct thread *t = thread_current ();
  if (t == idle_thread)
    ;
  /* mlfqs를 쓰면 here에서 recent_cpu/priority 업데이트 가능(단순화). */
}

/* 통계 출력(선택). */
void
thread_print_stats (void)
{
  printf ("Thread stats not implemented.\n");
}

/* 커널 스레드 시작 래퍼. */
static void
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);
  intr_enable ();       /* 인터럽트 허용 후 */
  function (aux);       /* 실제 함수 실행 */
  thread_exit ();       /* 끝나면 종료 */
}

/* 스레드 생성 */
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux)
{
  struct thread *t;
  tid_t tid;

  ASSERT (function != NULL);

  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* 새 스레드의 커널 스택을 설정하고, switch_entry로 진입하게 만듦. */
  /* (Pintos 기본 코드 그대로 유지) */
  extern void thread_create_setup_stack (struct thread *, thread_func *, void *);
  thread_create_setup_stack (t, function, aux);

  /* 준비 큐에 삽입(우선순위 정렬) */
  thread_unblock (t);

  /* 새 스레드가 더 우선순위가 높다면 양보 */
  if (thread_current ()->priority < t->priority)
    thread_yield ();

  return tid;
}

/* 현재 스레드 반환 */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();
  ASSERT (t != NULL);
  ASSERT (t->magic == THREAD_MAGIC);
  ASSERT (t->status == THREAD_RUNNING);
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

/* 현재 스레드 종료 */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

  enum intr_level old = intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* 현재 스레드 양보: ready_list(정렬)로 보내고 스케줄 */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old = intr_disable ();
  if (cur != idle_thread)
    {
      list_insert_ordered (&ready_list, &cur->elem,
                           thread_compare_priority, NULL);
      cur->status = THREAD_READY;
    }
  schedule ();
  intr_set_level (old);
}

/* 스레드 t를 BLOCKED->READY로 만들고 ready_list(정렬)에 넣음. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  t->status = THREAD_READY;

  /* 정렬 삽입: 높은 우선순위가 앞 */
  list_insert_ordered (&ready_list, &t->elem,
                       thread_compare_priority, NULL);
  intr_set_level (old_level);

  /* 더 높은 우선순위가 깨어났으면 양보 */
  if (thread_current ()->priority < t->priority)
    {
      if (intr_context ()) intr_yield_on_return ();
      else thread_yield ();
    }
}

/* 현재 스레드를 BLOCKED로 바꾼 뒤 스케줄 */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  enum intr_level old = intr_disable ();
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
  intr_set_level (old);
}

/* 다음에 실행할 스레드: ready_list의 맨 앞(최고 우선순위) */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* 스케줄러 핵심 */
static void
schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (next != NULL);

  if (cur == next)
    return;

  prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* 컨텍스트 스위치가 끝난 직후 마무리 작업 */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  cur->status = THREAD_RUNNING;

  if (prev != NULL && prev->status == THREAD_DYING && prev != cur)
    palloc_free_page (prev);
}

/* 초기화 루틴 */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (priority >= PRI_MIN && priority <= PRI_MAX);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *)t + PGSIZE;
  t->priority = priority;
  t->wake_tick = INT64_MAX;
  t->magic = THREAD_MAGIC;

  list_push_back (&all_list, &t->allelem);
}

/* 우선순위 Getter/Setter */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

void
thread_set_priority (int new_priority)
{
  enum intr_level old = intr_disable ();
  int old_pri = thread_current ()->priority;
  thread_current ()->priority = new_priority;

  /* 우선순위가 낮아졌다면 더 높은 놈에게 양보 */
  if (new_priority < old_pri && !list_empty (&ready_list))
    {
      struct thread *top =
        list_entry (list_front (&ready_list), struct thread, elem);
      if (top->priority > thread_current ()->priority)
        thread_yield ();
    }
  intr_set_level (old);
}

/* nice/mlfqs — 여기서는 단순 스텁로 둠(mlfqs-simplified는 패닉만 아니면 됨) */
int thread_get_nice (void)            { return 0; }
void thread_set_nice (int nice UNUSED) { /* no-op */ }
int thread_get_recent_cpu (void)      { return 0; }
int thread_get_load_avg (void)        { return 0; }

/* TID 할당기 */
static tid_t
allocate_tid (void)
{
  static tid_t next = 1;
  return next++;
}

