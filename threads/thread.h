#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* 스레드 상태. */
enum thread_status
  {
    THREAD_RUNNING,     /* 실행 중 */
    THREAD_READY,       /* CPU 대기(ready_list) */
    THREAD_BLOCKED,     /* 어떤 이벤트를 기다림 */
    THREAD_DYING        /* 소멸 직전 */
  };

/* 스레드 식별자 타입. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* 실패 시 반환 값. */

/* 우선순위 범위. */
#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

/* Forward decl. */
struct lock;

/* 스레드 구조체. */
struct thread
  {
    /* 커널 스택과 같은 페이지에 놓임. */
    tid_t tid;                          /* 스레드 ID. */
    enum thread_status status;          /* 상태. */
    char name[16];                      /* 디버깅용 이름. */
    uint8_t *stack;                     /* 커널 스택의 최상단 포인터. */
    int priority;                       /* 우선순위(기본). */

    /* ready_list, all_list에 들어갈 때 쓰는 링크 */
    struct list_elem elem;

    /* 알람 슬립용 타이머 틱 (깨울 시각). */
    int64_t wake_tick;

    /* Donation을 구현할 경우 여기에 donor 리스트/lock 포인터 등을 확장. */

    /* 모든 스레드 리스트(all_list)용 링크. */
    struct list_elem allelem;

    /* 사용자 프로그램용 필드들이나 파일 디스크립터 테이블 등이 이어짐… */

    /* Magic number for stack overflow detection. */
    unsigned magic;
  };

/* 글로벌 리스트들 (다른 모듈에서 참조할 수 있도록 extern) */
extern struct list ready_list;   /* 우선순위 정렬 ready 큐 */
extern struct list all_list;     /* 모든 스레드 */
/* 알람 슬립 대기 리스트(오름차순 wake_tick 정렬) — timer.c에서 사용 */
extern struct list sleep_list;

/* 스레드 서브시스템 */
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

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* 우선순위 조작 */
int thread_get_priority (void);
void thread_set_priority (int);

/* nice / mlfqs (필요 시) */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

/* ready_list 정렬 비교 함수: 높은 priority가 먼저 오도록 */
bool thread_compare_priority (const struct list_elem *a,
                              const struct list_elem *b,
                              void *aux);

/* sleep_list 정렬 비교 함수: wake_tick 오름차순 */
bool wake_tick_less (const struct list_elem *a,
                     const struct list_elem *b,
                     void *aux);

/* 현재 ticks 기준으로 슬립 리스트에서 깨울 애들 깨우기 */
void thread_wake (int64_t current_ticks);

extern bool thread_mlfqs;


#endif /* threads/thread.h */
