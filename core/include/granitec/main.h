/* The single writer's thread: what the main actor is to Swift Granite and Main to GraniteRust.
 *
 * PIN: A TURN NEVER WAITS. Every write to a run's record, every drain and every settle is one
 * synchronous turn on granite.main, in the order it was sent, so they never interleave. Work that
 * blocks runs on its own thread and sends back.
 * PIN: MAIN IS THE LIBRARY'S. Turns are GraniteC's own; the author's code never runs on main.
 *
 * The thread starts on first use and lives as long as the process. */
#ifndef GRANITEC_MAIN_H
#define GRANITEC_MAIN_H

#include <stdbool.h>

typedef void (*granite_turn)(void *arg);

/* Runs `turn(arg)` on main after everything sent before it. Always deferred, even from main. */
void granite_main_enqueue(granite_turn turn, void *arg);
/* Runs `turn(arg)` now when the caller is on main, else on main's next turn (Swift's onMain). */
void granite_main_run(granite_turn turn, void *arg);
/* Runs `turn(arg)` on main and returns once it has: inline when the caller is main. */
void granite_main_call(granite_turn turn, void *arg);
/* Returns once main has run every turn sent to it before the call. */
void granite_main_turn(void);
/* Whether the calling thread is granite.main. */
bool granite_main_is_current(void);

#endif
