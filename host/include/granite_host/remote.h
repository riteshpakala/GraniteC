/* What calling an agent in another process can say besides an answer: GraniteRust's
 * AgentRemoteError, in its sentences. */
#ifndef GRANITE_HOST_REMOTE_H
#define GRANITE_HOST_REMOTE_H

#include "granitec/error.h"

typedef enum granite_remote_type {
    GRANITE_REMOTE_FAILED,     /* the call failed, and said why */
    GRANITE_REMOTE_REFUSED,    /* refused, with its kind and reasons */
    GRANITE_REMOTE_FAULT,      /* it failed by its own fault */
    GRANITE_REMOTE_ENDED,      /* its process ended without answering */
    GRANITE_REMOTE_UNREADABLE, /* its process broke the wire */
    GRANITE_REMOTE_CHANGED,    /* its executable is not the one installed */
    GRANITE_REMOTE_SPAWN,      /* it could not start */
} granite_remote_type;

typedef struct granite_remote_error {
    granite_remote_type type;
    char *word;
    char *message; /* failed, refused, fault: what it said */
    granite_error *refusal;
    char *status; /* ended: "exit 1", "signal 6 (SIGABRT)" */
    char *log;    /* ended: the last lines it printed off the wire */
    char *reason; /* unreadable, spawn */
    char *path;   /* changed, spawn */
} granite_remote_error;

granite_remote_error *granite_remote_new(granite_remote_type type, const char *word);
/* The sentence, as Rust's Display says it. The caller frees it. */
char *granite_remote_text(const granite_remote_error *error);
void granite_remote_free(granite_remote_error *error);

#endif
