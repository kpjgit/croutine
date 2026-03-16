#define _POSIX_C_SOURCE 200809L
#include "croutine.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#endif

#define CO_MAGIC 0xC0C0BEEF
#define CO_ERR_IO_CONFLICT   (-1001)
#define CO_ERR_IO_BACKEND    (-1002)
#define CO_ERR_IO_BADFD      (-1003)
#define CO_ERR_WAIT_INVALID  (-1004)

static void q_push(co_frame_t** head, co_frame_t** tail, co_frame_t* fr) {
    fr->next = NULL;
    if (!*tail) {
        *head = *tail = fr;
    } else {
        (*tail)->next = fr;
        *tail = fr;
    }
}

static co_frame_t* q_pop(co_frame_t** head, co_frame_t** tail) {
    co_frame_t* fr = *head;
    if (!fr) return NULL;
    *head = fr->next;
    if (!*head) *tail = NULL;
    fr->next = NULL;
    return fr;
}

static void sleep_insert_sorted(co_sched_t* s, co_frame_t* fr) {
    fr->next = NULL;

    co_frame_t** cur = &s->sleep_head;
    while (*cur) {
        if ((*cur)->wait.timer.wake_ms > fr->wait.timer.wake_ms) break;
        cur = &(*cur)->next;
    }
    fr->next = *cur;
    *cur = fr;
}

static void wake_due_timers(co_sched_t* s) {
    if (!s->now_ms) return;
    uint64_t now = s->now_ms(s->now_ctx);

    while (s->sleep_head) {
        co_frame_t* fr = s->sleep_head;
        if (fr->wait.timer.wake_ms > now) break;

        s->sleep_head = fr->next;
        fr->next = NULL;

        fr->wait_kind = CO_WAIT_NONE;
        fr->status = CO_ST_READY;
        q_push(&s->ready_head, &s->ready_tail, fr);
    }
}

#ifdef __linux__
static uint32_t co_events_to_epoll(uint32_t events) {
    uint32_t out = 0;
    if (events & CO_IO_READ)  out |= (uint32_t)EPOLLIN;
    if (events & CO_IO_WRITE) out |= (uint32_t)EPOLLOUT;
    return out;
}

static uint32_t epoll_to_co_events(uint32_t ev) {
    uint32_t out = 0;
    if (ev & (uint32_t)EPOLLIN)  out |= CO_IO_READ;
    if (ev & (uint32_t)EPOLLOUT) out |= CO_IO_WRITE;
    if (ev & (uint32_t)EPOLLERR) out |= CO_IO_ERR;
    if (ev & (uint32_t)EPOLLHUP) out |= CO_IO_HUP;
    return out;
}

static int ensure_fd_capacity(co_sched_t* s, int fd) {
    if (fd < 0) return -1;
    size_t need = (size_t)fd + 1;
    if (need <= s->fd_waiters_cap) return 0;

    size_t new_cap = s->fd_waiters_cap ? s->fd_waiters_cap : 16;
    while (new_cap < need) new_cap *= 2;

    co_fd_waiters_t* nw = (co_fd_waiters_t*)realloc(s->fd_waiters, new_cap * sizeof(*nw));
    if (!nw) return -1;

    memset(nw + s->fd_waiters_cap, 0, (new_cap - s->fd_waiters_cap) * sizeof(*nw));
    s->fd_waiters = nw;
    s->fd_waiters_cap = new_cap;
    return 0;
}

static int update_epoll_interest(co_sched_t* s, int fd) {
    if (fd < 0 || (size_t)fd >= s->fd_waiters_cap) return -1;

    co_fd_waiters_t* slot = &s->fd_waiters[fd];
    uint32_t co_events = 0;
    if (slot->read_waiter)  co_events |= CO_IO_READ;
    if (slot->write_waiter) co_events |= CO_IO_WRITE;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    ev.events = co_events_to_epoll(co_events);

    if (co_events == 0) {
        if (epoll_ctl(s->epfd, EPOLL_CTL_DEL, fd, NULL) == -1 && errno != ENOENT) {
            return -1;
        }
        return 0;
    }

    if (epoll_ctl(s->epfd, EPOLL_CTL_MOD, fd, &ev) == 0) {
        return 0;
    }
    if (errno != ENOENT) {
        return -1;
    }
    if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        return -1;
    }
    return 0;
}

static int register_io_wait(co_sched_t* s, co_frame_t* fr) {
    int fd = fr->wait.io.fd;
    uint32_t events = fr->wait.io.events;

    if (s->epfd < 0) {
        fr->err = CO_ERR_IO_BACKEND;
        fr->status = CO_ST_ERROR;
        return -1;
    }
    if (fd < 0) {
        fr->err = CO_ERR_IO_BADFD;
        fr->status = CO_ST_ERROR;
        return -1;
    }
    if (ensure_fd_capacity(s, fd) != 0) {
        fr->err = ENOMEM;
        fr->status = CO_ST_ERROR;
        return -1;
    }

    co_fd_waiters_t* slot = &s->fd_waiters[fd];

    if (events & CO_IO_READ) {
        if (slot->read_waiter && slot->read_waiter != fr) {
            fr->err = CO_ERR_IO_CONFLICT;
            fr->status = CO_ST_ERROR;
            return -1;
        }
        if (!slot->read_waiter) {
            slot->read_waiter = fr;
            s->io_waiter_count++;
        }
    }

    if (events & CO_IO_WRITE) {
        if (slot->write_waiter && slot->write_waiter != fr) {
            if ((events & CO_IO_READ) && slot->read_waiter == fr) {
                slot->read_waiter = NULL;
                s->io_waiter_count--;
            }
            fr->err = CO_ERR_IO_CONFLICT;
            fr->status = CO_ST_ERROR;
            return -1;
        }
        if (!slot->write_waiter) {
            slot->write_waiter = fr;
            s->io_waiter_count++;
        }
    }

    if (update_epoll_interest(s, fd) != 0) {
        if ((events & CO_IO_READ) && slot->read_waiter == fr) {
            slot->read_waiter = NULL;
            s->io_waiter_count--;
        }
        if ((events & CO_IO_WRITE) && slot->write_waiter == fr) {
            slot->write_waiter = NULL;
            s->io_waiter_count--;
        }
        fr->err = errno ? -errno : CO_ERR_IO_BACKEND;
        fr->status = CO_ST_ERROR;
        return -1;
    }

    return 0;
}

static void wake_frame_from_io(co_sched_t* s, co_frame_t* fr, uint32_t revents) {
    fr->wait.io.revents |= revents;
    fr->wait_kind = CO_WAIT_NONE;
    fr->status = CO_ST_READY;
    q_push(&s->ready_head, &s->ready_tail, fr);
}
#endif

void co_sched_init(co_sched_t* s, co_now_ms_fn now_ms, void* now_ctx) {
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
    s->now_ms  = now_ms;
    s->now_ctx = now_ctx;
#ifdef __linux__
    s->epfd = epoll_create1(0);
#else
    s->epfd = -1;
#endif
}

void co_sched_close(co_sched_t* s) {
    if (!s) return;
#ifdef __linux__
    if (s->epfd >= 0) {
        close(s->epfd);
        s->epfd = -1;
    }
#endif
    free(s->fd_waiters);
    s->fd_waiters = NULL;
    s->fd_waiters_cap = 0;
    s->io_waiter_count = 0;
}

co_frame_t* co_frame_create(co_step_fn step,
                            size_t locals_size,
                            co_cleanup_fn cleanup,
                            const char* name) {
    if (!step) return NULL;

    co_frame_t* fr = (co_frame_t*)calloc(1, sizeof(co_frame_t));
    if (!fr) return NULL;

    fr->pc = 0;
    fr->status = CO_ST_READY;
    fr->err = 0;
    fr->step = step;
    fr->cleanup = cleanup;
    fr->locals_size = locals_size;
    if (locals_size) {
        fr->locals = calloc(1, locals_size);
        if (!fr->locals) {
            free(fr);
            return NULL;
        }
    }
    fr->wait_kind = CO_WAIT_NONE;
    fr->name = name ? name : "co";
    fr->magic = CO_MAGIC;
    return fr;
}

void co_frame_destroy(co_frame_t* fr) {
    if (!fr) return;
    if (fr->magic != CO_MAGIC) {
        return;
    }
    if (fr->cleanup) fr->cleanup(fr);
    free(fr->locals);
    fr->magic = 0;
    free(fr);
}

void co_spawn(co_sched_t* s, co_frame_t* fr) {
    if (!s || !fr) return;
    if (fr->magic != CO_MAGIC) return;

    fr->id = s->next_id++;
    fr->status = CO_ST_READY;
    q_push(&s->ready_head, &s->ready_tail, fr);
}

void co_cancel(co_frame_t* fr, int err_code) {
    if (!fr) return;
    fr->status = CO_ST_CANCELLED;
    fr->err = err_code;
}

int co_sched_poll_io(co_sched_t* s, int timeout_ms) {
    if (!s) return 0;
#ifdef __linux__
    if (s->epfd < 0 || s->io_waiter_count == 0) {
        return 0;
    }

    struct epoll_event events[16];
    int n = epoll_wait(s->epfd, events, 16, timeout_ms);
    if (n <= 0) {
        return 0;
    }

    int woken = 0;
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd < 0 || (size_t)fd >= s->fd_waiters_cap) continue;

        co_fd_waiters_t* slot = &s->fd_waiters[fd];
        uint32_t revents = epoll_to_co_events((uint32_t)events[i].events);

        co_frame_t* read_fr = NULL;
        co_frame_t* write_fr = NULL;

        if ((revents & (CO_IO_READ | CO_IO_ERR | CO_IO_HUP)) && slot->read_waiter) {
            read_fr = slot->read_waiter;
            slot->read_waiter = NULL;
            s->io_waiter_count--;
        }
        if ((revents & (CO_IO_WRITE | CO_IO_ERR | CO_IO_HUP)) && slot->write_waiter) {
            write_fr = slot->write_waiter;
            slot->write_waiter = NULL;
            s->io_waiter_count--;
        }

        (void)update_epoll_interest(s, fd);

        if (read_fr) {
            wake_frame_from_io(s, read_fr, revents);
            woken++;
        }
        if (write_fr && write_fr != read_fr) {
            wake_frame_from_io(s, write_fr, revents);
            woken++;
        }
    }
    return woken;
#else
    (void)timeout_ms;
    return 0;
#endif
}

int co_sched_has_work(const co_sched_t* s) {
    if (!s) return 0;
    return s->ready_head != NULL || s->sleep_head != NULL || s->io_waiter_count != 0;
}

int co_sched_next_timeout_ms(const co_sched_t* s, int default_timeout_ms) {
    if (!s || !s->sleep_head || !s->now_ms) return default_timeout_ms;
    uint64_t now = s->now_ms(s->now_ctx);
    uint64_t wake = s->sleep_head->wait.timer.wake_ms;
    if (wake <= now) return 0;
    uint64_t delta = wake - now;
    if (default_timeout_ms >= 0 && delta > (uint64_t)default_timeout_ms) {
        return default_timeout_ms;
    }
    if (delta > (uint64_t)INT32_MAX) return INT32_MAX;
    return (int)delta;
}

int co_sched_pump(co_sched_t* s, void* user_ctx) {
    if (!s) return 0;

    wake_due_timers(s);
    (void)co_sched_poll_io(s, 0);

    int stepped = 0;
    co_frame_t* fr;
    while ((fr = q_pop(&s->ready_head, &s->ready_tail)) != NULL) {
        if (fr->magic != CO_MAGIC) continue;

        if (fr->status == CO_ST_CANCELLED || fr->status == CO_ST_DONE || fr->status == CO_ST_ERROR) {
            co_frame_destroy(fr);
            continue;
        }

        stepped++;
        co_status_t st = fr->step(fr, user_ctx);
        fr->status = st;

        if (st == CO_ST_READY) {
            q_push(&s->ready_head, &s->ready_tail, fr);
        } else if (st == CO_ST_WAITING) {
            if (fr->wait_kind == CO_WAIT_TIMER) {
                sleep_insert_sorted(s, fr);
            } else if (fr->wait_kind == CO_WAIT_IO) {
#ifdef __linux__
                if (register_io_wait(s, fr) != 0) {
                    co_frame_destroy(fr);
                }
#else
                fr->err = CO_ERR_IO_BACKEND;
                co_frame_destroy(fr);
#endif
            } else {
                fr->err = CO_ERR_WAIT_INVALID;
                co_frame_destroy(fr);
            }
        } else {
            co_frame_destroy(fr);
        }
    }

    return stepped;
}
