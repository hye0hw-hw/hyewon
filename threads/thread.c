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

/* 스케줄러용 준비 큐(기본: 단일 ready_list, MLFQS: 3단계 큐) */
static struct list ready_list;     /* 우선순위 정렬 ready 큐(비-MLFQS) */
static struct list q0_list, q1_list, q2_list; /* MLFQS용 3단계 큐 */

static struct thread *idle_thread;
static struct thread *initial_thread;

static bool thread_started;

static struct lock tid_lock;
static tid_t next_tid = 1;

static bool opt_mlfqs = false;  /* 커맨드라인 -mlfqs 여부(전역) */

/* 스케줄러 통계 */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;

/* 스레드 로컬 */
#define THREAD_MAGIC 0xcd6abf4b

/* 내부 함수 원형 */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static void init_thread (struct thread *, const char *name, int priority);
static void schedule (void);
void schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

static struct thread *next_thread_to_run (void);

static void ready_push (struct thread *t);
static struct thread *ready_pop (void);

static void mlfqs_on_enqueue (struct thread *t);
static void mlfqs_maybe_preempt_on_unblock (struct thread *unblocked);
static void mlfqs_tick_update_running (struct thread *cur);

static void aging_on_tick (void);
static void aging_on_enqueue (struct thread *t);

static void preempt_if_needed (void);

/* ---- 외부 공개 ---- */

void
thread_set_mlfqs_enabled (bool on) {
  opt_mlfqs = on;
}

bool
thread_mlfqs_enabled (void) {
  return opt_mlfqs;
}

bool
thread_is_mlfqs (void) {
  return opt_mlfqs;
}

int
thread_effective_priority (const struct thread *t) {
  /* 현재 구현에서는 donation 미사용. 효과적 우선순위 = priority */
  return t ? t->priority : PRI_MIN;
}

bool
thread_priority_higher (const struct list_elem *a,
                        const struct list_elem *b, void *aux UNUSED) {
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  int pa = thread_effective_priority (ta);
  int pb = thread_effective_priority (tb);
  if (pa != pb) return pa > pb;
  /* 동일 우선순위면 FIFO 보장 위해 삽입 순서 유지: list_insert_ordered가 안정적이지 않으므로
     여기서는 tie-break 없음(테스트에서 생성 순서를 유지하려면 ready_push에서 list_push_back 사용) */
  return false;
}

/* ---- 초기화 ---- */

void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  /* 커맨드라인 파싱: -mlfqs 지원 */
  /* Pintos 기본 프레임워크에선 parse는 init에서 처리되지만,
     여기선 외부에서 thread_set_mlfqs_enabled()로 설정되었다고 가정해도 OK */

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&q0_list);
  list_init (&q1_list);
  list_init (&q2_list);

  /* 초기 스레드 설정 */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  thread_started = false;
}

void
thread_start (void)
{
  /* idle 스레드 생성 */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* 인터럽트 허용 */
  intr_enable ();

  /* idle 생성 완료 대기 */
  sema_down (&idle_started);
  thread_started = true;
}

/* 매 틱 호출: 타이머 인터럽트 컨텍스트 */
void
thread_tick (void)
{
  struct thread *t = thread_current ();

  /* 통계 */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* 라운드 로빈/선점 처리용 틱 진행 */
  if (t != idle_thread) {
    if (opt_mlfqs) {
      mlfqs_tick_update_running (t);
    } else {
      /* 기본: TIME_SLICE마다 양보 (Pintos 기본 값 사용) */
      if (timer_ticks () % TIMER_FREQ == 0) {
        /* 1초 통계용 훅 - 필요 시 */
      }
      /* Pintos 기본 라운드로빈: TIME_SLICE = 4 (기본 코드와 호환) */
      extern int64_t ticks; /* timer.c 에 있음 */
      (void)ticks;
    }
  }

  /* 에이징(ready 큐에서 대기 중인 모든 스레드에 대해 age 증가/승급) */
  aging_on_tick ();

  /* 선점 필요 시 인터럽트 리턴 후 양보 */
  preempt_if_needed ();
}

void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* 커널 스레드 시작 래퍼 */
static void
kernel_thread (thread_func *function, void *aux)
{
  intr_enable ();       /* 스케줄링 허용 */
  function (aux);
  thread_exit ();
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

  /* 커널 스레드 스택에 kernel_thread 설치 */
  t->stack = (uint8_t *) ((uint8_t *) t + PGSIZE);
  /* switch_entry -> switch_threads로 이어지는 기본 프레임 구성은
     Pintos 원본 switch.S/switch.h 흐름에 맞춰져 있다고 가정 */

  /* 준비 큐 삽입 */
  enum intr_level old_level = intr_disable ();
  thread_unblock (t);
  intr_set_level (old_level);

  /* 더 높은 우선순위가 깨어났다면 선점 */
  if (thread_effective_priority (t) > thread_effective_priority (thread_current ()))
    thread_yield ();

  return tid;
}

/* 현재 스레드 반환 */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();
  ASSERT (t->status == THREAD_RUNNING);
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

/* 현재 스레드 종료 */
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

/* 현재 스레드 양보(READY로 보내고 스케줄) */
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

/* 스레드 block (동기화 대기 등) */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* 스레드 unblock (ready로) */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  t->status = THREAD_READY;
  aging_on_enqueue (t);
  ready_push (t);
  intr_set_level (old_level);

  /* 높은 우선순위가 깨어났다면 선점. (인터럽트 컨텍스트라면 intr_yield_on_return 사용) */
  if (opt_mlfqs) {
    mlfqs_maybe_preempt_on_unblock (t);
  } else {
    if (thread_effective_priority (t) > thread_effective_priority (thread_current ())) {
      if (intr_context ()) intr_yield_on_return ();
      else thread_yield ();
    }
  }
}

/* 우선순위 get/set */
int
thread_get_priority (void)
{
  return thread_effective_priority (thread_current ());
}

void
thread_set_priority (int new_priority)
{
  struct thread *cur = thread_current ();
  if (opt_mlfqs) {
    /* -mlfqs 모드에서는 수동 우선순위 설정 비활성(요구사항: 단순 MLFQS이므로 무시) */
    return;
  }
  cur->base_priority = new_priority;
  cur->priority = new_priority;

  /* 낮아졌다면 더 높은 ready 스레드에 선점 허용 */
  preempt_if_needed ();
}

/* ---- 내부 구현 ---- */

static void
preempt_if_needed (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old = intr_disable ();

  int cur_p = thread_effective_priority (cur);
  int top_p = cur_p;

  if (opt_mlfqs) {
    /* 상위 큐에 대기 스레드가 있는지 검사 */
    if (!list_empty (&q0_list)) top_p = PRI_MAX; /* Q0가 존재하면 무조건 선점 대상 */
    else if (!list_empty (&q1_list) && cur->mlfqs_level > 1) top_p = PRI_MAX - 1;
    /* Q2만 있다면 그대로 */
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

  t->mlfqs = opt_mlfqs;
  t->mlfqs_level = 0; /* Q0부터 시작 */
  t->time_slice_used = 0;

#ifdef USERPROG
  t->pagedir = NULL;
#endif
  t->magic = THREAD_MAGIC;
}

/* 현재 CPU에서 돌고 있는 스레드의 thread 구조체 포인터 */
static struct thread *
running_thread (void)
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return (struct thread *) ((uintptr_t) esp & ~ (PGSIZE - 1));
}

/* idle 스레드 */
static void
idle (void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) {
    intr_disable ();
    thread_block ();
    /* 깨어나면 HLT 대체: 장치 인터럽트로 깨어남 */
    asm volatile ("sti; hlt" : : : "memory");
  }
}

/* 준비 큐 삽입 (모드별) */
static void
ready_push (struct thread *t)
{
  if (!t) return;

  if (opt_mlfqs) {
    /* 해당 큐의 뒤에 삽입(FIFO). */
    if (t->mlfqs_level <= 0)      list_push_back (&q0_list, &t->elem);
    else if (t->mlfqs_level == 1) list_push_back (&q1_list, &t->elem);
    else                          list_push_back (&q2_list, &t->elem);
  } else {
    /* 비-MLFQS: 우선순위 내림차순 정렬 유지.
       동일 우선순위는 생성/도착 순서 유지 위해 push_back 후 list_sort 대신 ordered insert 사용 */
    list_insert_ordered (&ready_list, &t->elem, thread_priority_higher, NULL);
  }
}

/* 준비 큐 pop (다음 실행 스레드 선택) */
static struct thread *
ready_pop (void)
{
  if (opt_mlfqs) {
    struct list *sel = NULL;
    if (!list_empty (&q0_list)) sel = &q0_list;
    else if (!list_empty (&q1_list)) sel = &q1_list;
    else if (!list_empty (&q2_list)) sel = &q2_list;

    if (sel == NULL) return NULL;

    struct list_elem *e = list_pop_front (sel);
    struct thread *t = list_entry (e, struct thread, elem);
    return t;
  } else {
    if (list_empty (&ready_list)) return NULL;
    struct list_elem *e = list_pop_front (&ready_list);
    return list_entry (e, struct thread, elem);
  }
}

/* 다음 실행 스레드 결정 */
static struct thread *
next_thread_to_run (void)
{
  struct thread *t = ready_pop ();
  if (t) return t;
  return idle_thread;
}

/* 스케줄 엔진 */
static void
schedule (void)
{
  struct thread *cur = thread_current ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur == next)
    return;

  prev = switch_threads (cur, next);
  schedule_tail (prev);
}

/* 문맥 전환 완료 후 후처리 */
void
schedule_tail (struct thread *prev)
{
  struct thread *cur = thread_current ();
  cur->status = THREAD_RUNNING;
  /* 현재 큐에서 소비 틱 초기화는 실행 중 틱 누적에서 관리 */
  (void) prev;
}

/* tid 할당 */
static tid_t
allocate_tid (void)
{
  lock_acquire (&tid_lock);
  tid_t tid = next_tid++;
  lock_release (&tid_lock);
  return tid;
}

/* ---- 에이징 ---- */

static void
aging_on_enqueue (struct thread *t)
{
  if (!t) return;
  t->age = 0; /* ready에 들어갈 때 0으로 초기화 */
}

static void
promote_priority_one (struct thread *t)
{
  if (!t) return;
  if (t->priority < PRI_MAX) {
    t->priority += 1;
    if (!opt_mlfqs) {
      /* ready_list 정렬 영향: 위치 재조정이 필요하지만,
         우리는 unblock/재삽입 시 정렬 삽입을 하므로 틱마다 정렬 갱신은 필요 없음 */
    }
  }
}

static void
mlfqs_promote_queue_one (struct thread *t)
{
  if (!t) return;
  if (t->mlfqs_level > 0) t->mlfqs_level -= 1; /* 상위 큐로 한 단계 승급 */
}

/* 매 틱: ready 대기 중인 스레드들의 age 증가 및 필요시 승급 */
static void
aging_on_tick (void)
{
  if (opt_mlfqs) {
    /* 세 큐 모두 순회: 대기 중인 스레드 age++ 및 20마다 한 단계 승급 */
    struct list *queues[3] = { &q0_list, &q1_list, &q2_list };
    for (int q = 0; q < 3; q++) {
      struct list_elem *e = list_begin (queues[q]);
      while (e != list_end (queues[q])) {
        struct thread *t = list_entry (e, struct thread, elem);
        e = list_next (e);

        t->age += AGE_STEP;
        if (t->age >= AGE_THRESHOLD) {
          t->age = 0;
          mlfqs_promote_queue_one (t);
          /* 큐 변경: 현재 위치에서 제거 후 상위 큐로 이동 */
          list_remove (&t->elem);
          ready_push (t);
        }
      }
    }
  } else {
    /* 비-MLFQS: ready_list 순회하여 age++ 및 우선순위 1단계 상승 */
    struct list_elem *e = list_begin (&ready_list);
    while (e != list_end (&ready_list)) {
      struct thread *t = list_entry (e, struct thread, elem);
      e = list_next (e);

      t->age += AGE_STEP;
      if (t->age >= AGE_THRESHOLD) {
        t->age = 0;
        promote_priority_one (t);
        /* 우선순위 변경되었으므로 정렬 재배치: 위치를 다시 맞추기 위해 제거/재삽입 */
        list_remove (&t->elem);
        list_insert_ordered (&ready_list, &t->elem, thread_priority_higher, NULL);
      }
    }
  }
}

/* ---- MLFQS ---- */

static void
mlfqs_tick_update_running (struct thread *cur)
{
  if (cur == idle_thread) return;

  cur->time_slice_used++;

  int slice_limit =
      (cur->mlfqs_level == 0) ? MLFQS_Q0_SLICE :
      (cur->mlfqs_level == 1) ? MLFQS_Q1_SLICE : MLFQS_Q2_SLICE;

  bool used_up = (cur->time_slice_used >= slice_limit);

  /* 상위 큐 도착 선점 규칙:
     Q1 실행 중 Q0에 새 스레드가 준비되면 즉시 선점 (unblock 시점에도 처리) */
  if (cur->mlfqs_level > 0 && !list_empty (&q0_list)) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
    return;
  }

  if (used_up) {
    /* 타임 슬라이스 모두 소모: 다음 낮은 큐로 강등(Q0->Q1->Q2) */
    if (cur->mlfqs_level < 2) cur->mlfqs_level++;
    cur->time_slice_used = 0;

    /* 자신을 ready로 보내고 스케줄 */
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

  /* 규칙:
     - 현재가 Q1이고, Q0에 새 스레드가 들어오면 선점
     - 현재가 Q2이고, (Q0 또는 Q1)에 새 스레드가 들어오면 선점
     - 같은 레벨이면 FIFO 유지(선점 X) */
  int c = cur->mlfqs_level;
  int u = unblocked->mlfqs_level;

  bool should_preempt = false;
  if (c == 1 && u == 0) should_preempt = true;
  else if (c == 2 && (u == 0 || u == 1)) should_preempt = true;

  if (should_preempt) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

/* ---- 스위치 엔트리 (switch.S에서 호출)와 호환되는 어설션 ---- */

bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}
