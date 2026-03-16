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
    CO_WAIT_IO,
    CO_WAIT_FUTURE,
} co_wait_kind_t;

enum {
    CO_IO_READ  = 1u << 0,
    CO_IO_WRITE = 1u << 1,
    CO_IO_ERR   = 1u << 2,
    CO_IO_HUP   = 1u << 3,
};

struct co_frame;
struct co_sched;

typedef co_status_t (*co_step_fn)(struct co_frame* fr, void* user_ctx);
typedef void        (*co_cleanup_fn)(struct co_frame* fr);

// ---------- Frame ----------

typedef struct co_frame {
    int pc;

    co_status_t status;
    int         err;

    co_step_fn    step;
    co_cleanup_fn cleanup;

    void*  locals;
    size_t locals_size;

    co_wait_kind_t wait_kind;
    union {
        struct {
            uint64_t wake_ms;
        } timer;
        struct {
            int      fd;
            uint32_t events;
            uint32_t revents;
        } io;
        struct {
            void* ptr;
        } future;
    } wait;

    struct co_frame* next;

    uint64_t    id;
    const char* name;

    uint32_t magic;
} co_frame_t;

// ---------- Scheduler ----------

typedef uint64_t (*co_now_ms_fn)(void* now_ctx);

typedef struct co_fd_waiters {
    co_frame_t* read_waiter;
    co_frame_t* write_waiter;
} co_fd_waiters_t;

typedef struct co_sched {
    co_frame_t* ready_head;
    co_frame_t* ready_tail;

    co_frame_t* sleep_head;

    uint64_t next_id;

    co_now_ms_fn now_ms;
    void*        now_ctx;

    // Linux epoll backend for stage 1 I/O waiting.
    int              epfd;
    co_fd_waiters_t* fd_waiters;
    size_t           fd_waiters_cap;
    size_t           io_waiter_count;
} co_sched_t;

// ---------- API ----------

void co_sched_init(co_sched_t* s, co_now_ms_fn now_ms, void* now_ctx);
void co_sched_close(co_sched_t* s);

co_frame_t* co_frame_create(co_step_fn step,
                            size_t locals_size,
                            co_cleanup_fn cleanup,
                            const char* name);

void co_frame_destroy(co_frame_t* fr);

void co_spawn(co_sched_t* s, co_frame_t* fr);

int  co_sched_pump(co_sched_t* s, void* user_ctx);
int  co_sched_poll_io(co_sched_t* s, int timeout_ms);
int  co_sched_has_work(const co_sched_t* s);
int  co_sched_next_timeout_ms(const co_sched_t* s, int default_timeout_ms);

void co_cancel(co_frame_t* fr, int err_code);

// ---------- Helpers for coroutine bodies (protothreads style) ----------

#define CO_BEGIN(fr) switch ((fr)->pc) { case 0:

#define CO_YIELD(fr) do {                 \
    (fr)->pc = __LINE__;                  \
    return CO_ST_READY;                   \
    case __LINE__:;                       \
} while (0)

#define CO_AWAIT_UNTIL(fr, cond) do {     \
    (fr)->pc = __LINE__;                  \
    case __LINE__:                        \
    if (!(cond)) return CO_ST_WAITING;    \
} while (0)

#define CO_WAIT_IO(fr, fd_, events_) do {       \
    (fr)->wait_kind = CO_WAIT_IO;               \
    (fr)->wait.io.fd = (fd_);                   \
    (fr)->wait.io.events = (uint32_t)(events_); \
    (fr)->wait.io.revents = 0;                  \
    (fr)->pc = __LINE__;                        \
    return CO_ST_WAITING;                       \
    case __LINE__:;                             \
} while (0)

#define CO_WAIT_READ(fr, fd_)  CO_WAIT_IO((fr), (fd_), CO_IO_READ)
#define CO_WAIT_WRITE(fr, fd_) CO_WAIT_IO((fr), (fd_), CO_IO_WRITE)

#define CO_END(fr) } (fr)->status = CO_ST_DONE; return CO_ST_DONE

#ifdef __cplusplus
}
#endif
