/* threads/thread.c */

#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"

#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b
#define TIME_SLICE 4

static struct list ready_list;
static struct list all_list;
static struct list sleep_list;
static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;
static unsigned thread_ticks;

struct kernel_thread_frame
{
    void *eip;                  /* Return address */
    thread_func *function;      /* 실행할 함수 포인터 */
    void *aux;                  /* 인자 */
};

/* 스레드 전환 시 사용되는 프레임 구조체 */
struct switch_entry_frame
{
    void *eip; /* 실행 위치 */
};

struct switch_threads_frame
{
    void *eip;
    void *ebp;
};

/* 내부 함수 프로토타입 */
static void idle(void *aux UNUSED);
static bool is_thread(struct thread *t);
static void *alloc_frame(struct thread *t, size_t size);

bool thread_mlfqs;
static long long idle_ticks, kernel_ticks, user_ticks;

static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *, int);
static void schedule(void);
static void kernel_thread(thread_func *, void *aux);
static tid_t allocate_tid(void);
void thread_schedule_tail(struct thread *prev);

static struct list mlfqs_ready_queues[3];
static int64_t next_tick_to_wakeup = INT64_MAX;

/* 우선순위 비교 */
bool
thread_priority_less_func(const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux UNUSED)
{
    struct thread *t1 = list_entry(a, struct thread, elem);
    struct thread *t2 = list_entry(b, struct thread, elem);
    return t1->priority > t2->priority;
}

/* 초기화 */
void
thread_init(void)
{
    ASSERT(intr_get_level() == INTR_OFF);

    lock_init(&tid_lock);
    list_init(&ready_list);
    list_init(&all_list);
    list_init(&sleep_list);

    if (thread_mlfqs)
    {
        list_init(&mlfqs_ready_queues[0]);
        list_init(&mlfqs_ready_queues[1]);
        list_init(&mlfqs_ready_queues[2]);
    }

    initial_thread = running_thread();
    init_thread(initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid();
}

/* 스케줄 시작 */
void
thread_start(void)
{
    struct semaphore idle_started;
    sema_init(&idle_started, 0);
    thread_create("idle", PRI_MIN, idle, &idle_started);

    intr_enable();
    sema_down(&idle_started);
}

/* 타이머 틱마다 호출 */
void
thread_tick(void)
{
    struct thread *t = thread_current();

    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    if (thread_mlfqs)
    {
        /* MLFQS aging / demotion / preemption 부분은 네가 쓰던 코드 그대로 유지 */
        // (이 부분은 생략 가능, 이미 네 버전에서 완성돼 있음)
    }
    else
    {
        if (++thread_ticks >= TIME_SLICE)
            intr_yield_on_return();
    }
}

/* 현재 실행 중인 스레드 */
struct thread *
thread_current(void)
{
    struct thread *t = running_thread();
    ASSERT(is_thread(t));
    ASSERT(t->status == THREAD_RUNNING);
    return t;
}

/* 현재 스레드 이름 */
const char *
thread_name(void)
{
    return thread_current()->name;
}

/* 스레드 생성 */
tid_t
thread_create(const char *name, int priority, thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;

    ASSERT(function != NULL);

    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    init_thread(t, name, priority);
    tid = t->tid = allocate_tid();

    kf = alloc_frame(t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    ef = alloc_frame(t, sizeof *ef);
    ef->eip = (void (*)(void))kernel_thread;

    sf = alloc_frame(t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    thread_unblock(t);
    if (t->priority > thread_current()->priority)
        thread_yield();

    return tid;
}

/* 현재 스레드 차단 */
void
thread_block(void)
{
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);

    thread_current()->status = THREAD_BLOCKED;
    schedule();
}

/* 스레드 언블록 */
void
thread_unblock(struct thread *t)
{
    enum intr_level old_level;

    ASSERT(is_thread(t));

    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);
    list_insert_ordered(&ready_list, &t->elem, thread_priority_less_func, NULL);
    t->status = THREAD_READY;
    intr_set_level(old_level);
}

/* 스레드 양보 */
void
thread_yield(void)
{
    struct thread *cur = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (cur != idle_thread)
        list_insert_ordered(&ready_list, &cur->elem, thread_priority_less_func, NULL);
    cur->status = THREAD_READY;
    schedule();
    intr_set_level(old_level);
}

/* 스레드 종료 */
void
thread_exit(void)
{
    ASSERT(!intr_context());

#ifdef USERPROG
    process_exit();
#endif

    intr_disable();
    list_remove(&thread_current()->allelem);
    thread_current()->status = THREAD_DYING;
    schedule();
    NOT_REACHED();
}

/* 다음 실행 스레드 */
static struct thread *
next_thread_to_run(void)
{
    if (list_empty(&ready_list))
        return idle_thread;
    else
        return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* 스케줄링 */
static void
schedule(void)
{
    struct thread *cur = running_thread();
    struct thread *next = next_thread_to_run();
    struct thread *prev = NULL;

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(cur->status != THREAD_RUNNING);

    if (cur != next)
        prev = switch_threads(cur, next);
    thread_schedule_tail(prev);
}

/* 현재 실행 중인 스레드 반환 */
static struct thread *
running_thread(void)
{
    uint32_t *esp;
    asm("mov %%esp, %0" : "=g"(esp));
    return pg_round_down(esp);
}

/* TID 할당 */
static tid_t
allocate_tid(void)
{
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_lock);
    tid = next_tid++;
    lock_release(&tid_lock);

    return tid;
}

/* 스레드 초기화 */
static void
init_thread(struct thread *t, const char *name, int priority)
{
    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy(t->name, name, sizeof t->name);
    t->stack = (uint8_t *)t + PGSIZE;
    t->priority = priority;
    t->age = 0;
    t->mlfqs_level = 0;
    t->ticks_in_slice = 0;
    t->magic = THREAD_MAGIC;

    list_push_back(&all_list, &t->allelem);
}

/* 커널 스레드 실행 */
static void
kernel_thread(thread_func *function, void *aux)
{
    ASSERT(function != NULL);
    intr_enable();
    function(aux);
    thread_exit();
}
