#include "croutine.h"
#include <stdlib.h>
#include <string.h>

#define CO_MAGIC 0xC0C0BEEF

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

// Insert into sleep list sorted by wake_ms (ascending).
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

        // remove from sleep list
        s->sleep_head = fr->next;
        fr->next = NULL;

        fr->wait_kind = CO_WAIT_NONE;
        fr->status = CO_ST_READY;
        q_push(&s->ready_head, &s->ready_tail, fr);
    }
}

void co_sched_init(co_sched_t* s, co_now_ms_fn now_ms, void* now_ctx) {
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
    s->now_ms  = now_ms;
    s->now_ctx = now_ctx;
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
    fr->locals = NULL;
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
    // debug guard
    if (fr->magic != CO_MAGIC) {
        // double free / corruption hint; don't crash here in skeleton
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

int co_sched_pump(co_sched_t* s, void* user_ctx) {
    if (!s) return 0;

    // wake timers into ready queue
    wake_due_timers(s);

    int stepped = 0;

    // step a batch: here we drain all ready frames once
    // (you can cap iterations for fairness if you want)
    co_frame_t* fr;
    while ((fr = q_pop(&s->ready_head, &s->ready_tail)) != NULL) {
        if (fr->magic != CO_MAGIC) continue;

        // Skip if cancelled/done
        if (fr->status == CO_ST_CANCELLED || fr->status == CO_ST_DONE || fr->status == CO_ST_ERROR) {
            co_frame_destroy(fr);
            continue;
        }

        stepped++;

        co_status_t st = fr->step(fr, user_ctx);
        fr->status = st;

        if (st == CO_ST_READY) {
            // cooperative yield: back to ready
            q_push(&s->ready_head, &s->ready_tail, fr);
        } else if (st == CO_ST_WAITING) {
            // waiting: route by wait_kind
            if (fr->wait_kind == CO_WAIT_TIMER) {
                sleep_insert_sorted(s, fr);
            } else {
                // placeholder: if you don't set wait_kind, it would stall forever.
                // For now, treat "WAITING but no wait_kind" as READY to avoid deadlock.
                fr->status = CO_ST_READY;
                q_push(&s->ready_head, &s->ready_tail, fr);
            }
        } else {
            // DONE / ERROR / CANCELLED -> destroy
            co_frame_destroy(fr);
        }
    }

    return stepped;
}