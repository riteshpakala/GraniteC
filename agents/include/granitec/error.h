/* Why a call was not answered: a fault said as it is, a refusal with a kind a host can count and
 * reasons that each say their fix, or a cancel. GraniteRust's AgentRefusal, SaidRefusal and the
 * package errors, as one value.
 *
 * PIN: A REFUSAL IS NOT A FAULT. Only a refusal is the call's to correct; anything else a run
 * returns is the agent's own fault, said as it is.
 * PIN: AN ERROR HAS ONE OWNER. A run returns it, the engine hands it on, whoever reads it last
 * frees it with granite_error_free. */
#ifndef GRANITEC_ERROR_H
#define GRANITEC_ERROR_H

#include <stdarg.h>
#include <stddef.h>

/* Whose a refusal is: what a host tells its model. */
typedef enum granite_refusal_kind {
    GRANITE_REFUSAL_INPUT,  /* the call's JSON does not read as the input */
    GRANITE_REFUSAL_CALL,   /* the call cannot be honoured as written: the fix is the caller's */
    GRANITE_REFUSAL_BUILD,  /* a build the call needs failed */
    GRANITE_REFUSAL_DEVICE, /* a device the call needs failed: a GPU, a simulator */
    GRANITE_REFUSAL_MODEL,  /* the host's model gave nothing usable */
    GRANITE_REFUSAL_PARSE,  /* code the package wrote did not parse, so nothing was written */
    GRANITE_REFUSAL_REACH,  /* a service it reads could not be reached: try later */
    GRANITE_REFUSAL_ANSWER, /* its answer broke granite-answer/1: the package's own bug */
    GRANITE_REFUSAL_HOST,   /* the host could not serve it; also what an unknown kind reads as */
} granite_refusal_kind;

typedef enum granite_error_type {
    GRANITE_ERROR_FAULT,
    GRANITE_ERROR_REFUSED,
    GRANITE_ERROR_CANCELLED,
} granite_error_type;

typedef struct granite_error {
    granite_error_type type;
    /* The sentence it is said in. */
    char *said;
    /* A refusal's kind and reasons. */
    granite_refusal_kind kind;
    char **reasons;
    size_t reason_count;
} granite_error;

/* The run's own fault, said as it is. */
granite_error *granite_fault(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A refusal of one reason, said as that reason. */
granite_error *granite_refuse(granite_refusal_kind kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* A refusal of several reasons (copied), said as a count and a list when there is more than one. */
granite_error *granite_refuse_reasons(granite_refusal_kind kind, const char *const *reasons, size_t count);
/* A refusal said in the package's own sentence (Rust's AgentRefusing::refused): `said` is what a
 * person reads, `reasons` what a host counts. */
granite_error *granite_refuse_said(granite_refusal_kind kind, const char *said, const char *const *reasons,
                                   size_t count);
granite_error *granite_cancelled(void);
granite_error *granite_error_copy(const granite_error *error);
void granite_error_free(granite_error *error);

/* "input", "call", …; and back, where a kind this reader does not know reads as host. */
const char *granite_refusal_kind_name(granite_refusal_kind kind);
granite_refusal_kind granite_refusal_kind_named(const char *name);
/* How a refusal reads: one reason as it is, several as "N problems, so nothing was done:" and a
 * list. The caller frees it. */
char *granite_refusal_text(const char *const *reasons, size_t count);

/* What the host's model can say besides an answer, each a model refusal in Rust's sentence. */
granite_error *granite_host_no_model(const char *word);
granite_error *granite_host_no_answer(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
granite_error *granite_host_bad_answer(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
