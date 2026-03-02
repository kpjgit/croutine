#define _POSIX_C_SOURCE 200809L

#include "croutine.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t now_ms(void* _) {
    (void)_;
    return (uint64_t)GetTickCount64();
}
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
#include <unistd.h>
static uint64_t now_ms(void* _) {
    (void)_;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

typedef struct {
    int i;
} my_locals_t;

static co_status_t hello_step(co_frame_t* fr, void* user_ctx) {
    (void)user_ctx;
    my_locals_t* L = (my_locals_t*)fr->locals;

    CO_BEGIN(fr);

    for (L->i = 0; L->i < 3; L->i++) {
        printf("[%-8s] i=%d (yield)\n", fr->name, L->i);
        CO_YIELD(fr);
    }

    // wait 500ms using scheduler timer
    fr->wait_kind = CO_WAIT_TIMER;
    fr->wait.timer.wake_ms = now_ms(NULL) + 500;
    printf("[%-8s] waiting 500ms...\n", fr->name);
    CO_AWAIT_UNTIL(fr, now_ms(NULL) >= fr->wait.timer.wake_ms);

    printf("[%-8s] done!\n", fr->name);

    CO_END(fr);
}

int main(void) {
    co_sched_t S;
    co_sched_init(&S, now_ms, NULL);

    co_frame_t* a = co_frame_create(hello_step, sizeof(my_locals_t), NULL, "A");
    co_frame_t* b = co_frame_create(hello_step, sizeof(my_locals_t), NULL, "B");

    co_spawn(&S, a);
    co_spawn(&S, b);

    // crude "event loop"
    while (1) {
        int n = co_sched_pump(&S, NULL);
        if (n == 0 && S.sleep_head == NULL) break;
        sleep_ms(10);
    }

    return 0;
}