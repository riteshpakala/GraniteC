/* Threads with a name and a stack size, and the clock. See granitec/thread.h. */
#if defined(__linux__)
#define _GNU_SOURCE /* pthread_setname_np */
#endif
#include "granitec/thread.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "granitec/buf.h"

struct start {
    void (*body)(void *);
    void *arg;
    char name[16];
};

static void *trampoline(void *pointer) {
    struct start start = *(struct start *)pointer;
    free(pointer);
    granite_thread_name(start.name);
    start.body(start.arg);
    return NULL;
}

bool granite_thread_spawn(const char *name, size_t stack, void (*body)(void *), void *arg) {
    struct start *start = granite_calloc(1, sizeof *start);
    start->body = body;
    start->arg = arg;
    snprintf(start->name, sizeof start->name, "%s", name ? name : "granite");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stack) {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) page = 4096;
        if (stack < (size_t)PTHREAD_STACK_MIN) stack = (size_t)PTHREAD_STACK_MIN;
        stack = (stack + (size_t)page - 1) / (size_t)page * (size_t)page;
        pthread_attr_setstacksize(&attributes, stack);
    }
    pthread_t thread;
    int status = pthread_create(&thread, &attributes, trampoline, start);
    pthread_attr_destroy(&attributes);
    if (status != 0) {
        free(start);
        errno = status;
        return false;
    }
    return true;
}

void granite_thread_name(const char *name) {
    char shortened[16];
    snprintf(shortened, sizeof shortened, "%s", name ? name : "granite");
#if defined(__APPLE__)
    pthread_setname_np(shortened);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), shortened);
#endif
}

double granite_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void granite_sleep(double seconds) {
    if (seconds <= 0) return;
    struct timespec wait = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    struct timespec left;
    while (nanosleep(&wait, &left) != 0 && errno == EINTR) wait = left;
}
