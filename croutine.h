#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------- Status / Wait ----------

typedef enum {
    CO_ST_READY = 0,
    CO_ST_WAITING,
    CO_ST_DONE,
    CO_ST_ERROR,
    CO_ST_CANCELLED,
} co_status_t;

typedef enum {
    CO_WAIT_NONE = 0,
    CO_WAIT_TIMER,
    CO_WAIT_IO,      // 자리만: 나중에 epoll/select 붙일 때 확장
    CO_WAIT_FUTURE,  // 자리만
} co_wait_kind_t;

struct co_frame;
struct co_sched;

typedef co_status_t (*co_step_fn)(struct co_frame* fr, void* user_ctx);
typedef void        (*co_cleanup_fn)(struct co_frame* fr);

// ---------- Frame ----------

typedef struct co_frame {
    // protothreads resume point
    int pc;

    // lifecycle
    co_status_t status;
    int         err;         // optional error code

    // coroutine code
    co_step_fn    step;
    co_cleanup_fn cleanup;

    // user locals
    void*  locals;
    size_t locals_size;

    // wait state (union)
    co_wait_kind_t wait_kind;
    union {
        struct { uint64_t wake_ms; } timer;
        struct { int fd; int events; } io;     // placeholder
        struct { void* ptr; } future;          // placeholder
    } wait;

    // scheduler intrusive link
    struct co_frame* next;

    // debug / observability
    uint64_t    id;
    const char* name;

    // debug guards (optional)
    uint32_t magic;
} co_frame_t;

// ---------- Scheduler ----------

typedef uint64_t (*co_now_ms_fn)(void* now_ctx);

typedef struct co_sched {
    co_frame_t* ready_head;
    co_frame_t* ready_tail;

    // sleep list (sorted by wake_ms)
    co_frame_t* sleep_head;

    // id generator
    uint64_t next_id;

    // time source
    co_now_ms_fn now_ms;
    void*        now_ctx;
} co_sched_t;

// ---------- API ----------

void co_sched_init(co_sched_t* s, co_now_ms_fn now_ms, void* now_ctx);

co_frame_t* co_frame_create(co_step_fn step,
                            size_t locals_size,
                            co_cleanup_fn cleanup,
                            const char* name);

void co_frame_destroy(co_frame_t* fr);

void co_spawn(co_sched_t* s, co_frame_t* fr);

// run "some work". returns number of frames stepped.
int  co_sched_pump(co_sched_t* s, void* user_ctx);

// optional: cancel
void co_cancel(co_frame_t* fr, int err_code);

// ---------- Helpers for coroutine bodies (protothreads style) ----------

#define CO_BEGIN(fr) switch ((fr)->pc) { case 0:

// Yield control back to scheduler, remain READY (re-enqueue).
#define CO_YIELD(fr) do {                 \
    (fr)->pc = __LINE__;                  \
    return CO_ST_READY;                   \
    case __LINE__:;                       \
} while (0)

// Wait until condition becomes true; frame remains WAITING.
// Scheduler must re-step it later (e.g., timer wakes it or external event).
#define CO_AWAIT_UNTIL(fr, cond) do {     \
    (fr)->pc = __LINE__;                  \
    case __LINE__:                        \
    if (!(cond)) return CO_ST_WAITING;    \
} while (0)

#define CO_END(fr) } (fr)->status = CO_ST_DONE; return CO_ST_DONE

#ifdef __cplusplus
}
#endif