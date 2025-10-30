#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* 스레드 상태. */
enum thread_status {
  THREAD_RUNNING,     /* Running. */
  THREAD_READY,       /* Ready to run. */
  THREAD_BLOCKED,     /* Waiting for an event to trigger. */
  THREAD_DYING        /* About to be destroyed. */
};

/* 스레드 식별자 타입. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t error value. */

/* 기본 우선순위. (Pintos 기본 값과 호환) */
#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

/* ---- 단순 MLFQS 매개변수 ---- */
#define MLFQS_Q0_SLICE 2   /* 틱 */
#define MLFQS_Q1_SLICE 4   /* 틱 */
#define MLFQS_Q2_SLICE 8   /* 틱 */

#define AGE_THRESHOLD 20   /* 에이징 카운트 임계값 */
#define AGE_STEP 1         /* 틱당 age 증가량 */

struct thread {
  /* Owned by thread.c. */
  tid_t tid;                          /* Thread identifier. */
  enum thread_status status;          /* Thread state. */
  char name[16];                      /* Name (for debugging purposes). */
  uint8_t *stack;                     /* Saved stack pointer. */
  int priority;                       /* 현재(효과적) 우선순위. */
  int base_priority;                  /* 기본 우선순위(thread_set_priority용 기준). */
  int age;                            /* ready 대기 중 에이징 카운터. */

  bool mlfqs;                         /* -mlfqs 사용 여부(스레드 생성 시 시스템 전역 설정 복사). */
  int mlfqs_level;                    /* 0,1,2 중 하나 (Q0/Q1/Q2). */
  int time_slice_used;                /* 현재 큐에서 소비한 틱 수. */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem;              /* List element. Ready list or semaphore wait list. */

#ifdef USERPROG
  /* Owned by userprog/process.c. */
  uint32_t *pagedir;                  /* Page directory. */
#endif

  /* Owned by thread.c. */
  unsigned magic;                     /* Detects stack overflow. */
};

/* 스레드 API (원형) */
void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_yield (void);
void thread_exit (void) NO_RETURN;

int thread_get_priority (void);
void thread_set_priority (int);

bool thread_is_mlfqs (void);

/* 내부 비교자/유틸 함수 (synch에서도 사용) */
bool thread_priority_higher (const struct list_elem *, const struct list_elem *, void *);
int thread_effective_priority (const struct thread *t);

/* 테스트 헬퍼(옵션) */
void thread_set_mlfqs_enabled (bool on);
bool thread_mlfqs_enabled (void);

extern bool thread_mlfqs;           // 방법1을 선택했다면 필요 (init.c가 씀)
bool is_thread(struct thread *t);   // 사용 전에 원형 선언 필요

#endif /* threads/thread.h */
