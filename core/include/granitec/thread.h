/* Threads with a name and a stack size, and the clock GraniteC measures with. */
#ifndef GRANITEC_THREAD_H
#define GRANITEC_THREAD_H

#include <stdbool.h>
#include <stddef.h>

/* Starts `body(arg)` on a detached thread named `name` (shortened to 15 bytes, Linux's limit),
 * with a stack of `stack` bytes (0: the system's). False, with errno set, when it could not. */
bool granite_thread_spawn(const char *name, size_t stack, void (*body)(void *), void *arg);
/* Names the calling thread. */
void granite_thread_name(const char *name);

/* Seconds on a clock that never goes back. */
double granite_now(void);
/* Sleeps `seconds`, through signals. */
void granite_sleep(double seconds);

#endif
