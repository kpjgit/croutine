#define _POSIX_C_SOURCE 200809L

#include "croutine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    int read_fd;
    int write_fd;
} app_ctx_t;

static uint64_t now_ms(void* _) {
    (void)_;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    int      sent;
    uint64_t wake_ms;
} writer_locals_t;

static co_status_t writer_step(co_frame_t* fr, void* user_ctx) {
    app_ctx_t* app = (app_ctx_t*)user_ctx;
    writer_locals_t* L = (writer_locals_t*)fr->locals;
    static const char msg[] = "hello from writer coroutine\n";

    CO_BEGIN(fr);

    L->wake_ms = now_ms(NULL) + 500;
    fr->wait_kind = CO_WAIT_TIMER;
    fr->wait.timer.wake_ms = L->wake_ms;
    printf("[%-8s] sleep 500ms before write\n", fr->name);
    CO_AWAIT_UNTIL(fr, now_ms(NULL) >= L->wake_ms);

    while (!L->sent) {
        ssize_t n = write(app->write_fd, msg, sizeof(msg) - 1);
        if (n > 0) {
            printf("[%-8s] wrote %zd bytes\n", fr->name, n);
            L->sent = 1;
            break;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            printf("[%-8s] write would block, waiting...\n", fr->name);
            CO_WAIT_WRITE(fr, app->write_fd);
            continue;
        }
        perror("writer write");
        fr->err = errno;
        return CO_ST_ERROR;
    }

    CO_END(fr);
}

typedef struct {
    char   buf[128];
    size_t used;
} reader_locals_t;

static co_status_t reader_step(co_frame_t* fr, void* user_ctx) {
    app_ctx_t* app = (app_ctx_t*)user_ctx;
    reader_locals_t* L = (reader_locals_t*)fr->locals;

    CO_BEGIN(fr);

    while (1) {
        ssize_t n = read(app->read_fd, L->buf + L->used, sizeof(L->buf) - 1 - L->used);
        if (n > 0) {
            L->used += (size_t)n;
            L->buf[L->used] = '\0';
            printf("[%-8s] read %zd bytes: %s", fr->name, n, L->buf);
            break;
        }
        if (n == 0) {
            printf("[%-8s] eof\n", fr->name);
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("[%-8s] read would block, waiting...\n", fr->name);
            CO_WAIT_READ(fr, app->read_fd);
            continue;
        }
        perror("reader read");
        fr->err = errno;
        return CO_ST_ERROR;
    }

    CO_END(fr);
}

int main(void) {
#ifndef __linux__
    fprintf(stderr, "io_test.c stage-1 backend is Linux/epoll only.\n");
    return 1;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 1;
    }
    if (set_nonblocking(pipefd[0]) != 0 || set_nonblocking(pipefd[1]) != 0) {
        perror("fcntl(O_NONBLOCK)");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    app_ctx_t app = {
        .read_fd = pipefd[0],
        .write_fd = pipefd[1],
    };

    co_sched_t S;
    co_sched_init(&S, now_ms, NULL);

    co_frame_t* reader = co_frame_create(reader_step, sizeof(reader_locals_t), NULL, "reader");
    co_frame_t* writer = co_frame_create(writer_step, sizeof(writer_locals_t), NULL, "writer");
    if (!reader || !writer) {
        fprintf(stderr, "failed to create frames\n");
        close(pipefd[0]);
        close(pipefd[1]);
        co_sched_close(&S);
        return 1;
    }

    co_spawn(&S, reader);
    co_spawn(&S, writer);

    while (co_sched_has_work(&S)) {
        int stepped = co_sched_pump(&S, &app);
        if (stepped == 0) {
            int timeout_ms = co_sched_next_timeout_ms(&S, 1000);
            (void)co_sched_poll_io(&S, timeout_ms);
        }
    }

    close(pipefd[0]);
    close(pipefd[1]);
    co_sched_close(&S);
    return 0;
#endif
}
