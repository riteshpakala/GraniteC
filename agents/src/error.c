/* Why a call was not answered. See granitec/error.h. */
#include "granitec/error.h"

#include <stdlib.h>
#include <string.h>

#include "granitec/buf.h"

static const char *const KIND_NAMES[] = {"input", "call", "build", "device", "model", "parse", "reach", "answer", "host"};

const char *granite_refusal_kind_name(granite_refusal_kind kind) {
    return (unsigned)kind <= GRANITE_REFUSAL_HOST ? KIND_NAMES[kind] : "host";
}

granite_refusal_kind granite_refusal_kind_named(const char *name) {
    for (int kind = GRANITE_REFUSAL_INPUT; name && kind <= GRANITE_REFUSAL_HOST; kind++)
        if (strcmp(KIND_NAMES[kind], name) == 0) return (granite_refusal_kind)kind;
    return GRANITE_REFUSAL_HOST;
}

char *granite_refusal_text(const char *const *reasons, size_t count) {
    if (count == 1) return granite_strdup(reasons[0]);
    granite_buf text = {0};
    granite_buf_appendf(&text, "%zu problems, so nothing was done:", count);
    for (size_t i = 0; i < count; i++) granite_buf_appendf(&text, "\n- %s", reasons[i]);
    return granite_buf_take(&text);
}

static granite_error *make(granite_error_type type, char *said) {
    granite_error *error = granite_calloc(1, sizeof *error);
    error->type = type;
    error->said = said;
    return error;
}

granite_error *granite_fault(const char *fmt, ...) {
    granite_buf said = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&said, fmt, args);
    va_end(args);
    return make(GRANITE_ERROR_FAULT, granite_buf_take(&said));
}

granite_error *granite_refuse_said(granite_refusal_kind kind, const char *said, const char *const *reasons,
                                   size_t count) {
    granite_error *error = make(GRANITE_ERROR_REFUSED, granite_strdup(said));
    error->kind = kind;
    error->reasons = granite_calloc(count ? count : 1, sizeof *error->reasons);
    for (size_t i = 0; i < count; i++) error->reasons[i] = granite_strdup(reasons[i]);
    error->reason_count = count;
    return error;
}

granite_error *granite_refuse_reasons(granite_refusal_kind kind, const char *const *reasons, size_t count) {
    char *said = granite_refusal_text(reasons, count);
    granite_error *error = granite_refuse_said(kind, said, reasons, count);
    free(said);
    return error;
}

static granite_error *refuse_one(granite_refusal_kind kind, char *reason) {
    const char *reasons[] = {reason};
    granite_error *error = granite_refuse_reasons(kind, reasons, 1);
    free(reason);
    return error;
}

granite_error *granite_refuse(granite_refusal_kind kind, const char *fmt, ...) {
    granite_buf reason = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&reason, fmt, args);
    va_end(args);
    return refuse_one(kind, granite_buf_take(&reason));
}

granite_error *granite_cancelled(void) {
    return make(GRANITE_ERROR_CANCELLED, granite_strdup("cancelled"));
}

granite_error *granite_error_copy(const granite_error *error) {
    if (!error) return NULL;
    if (error->type != GRANITE_ERROR_REFUSED) return make(error->type, granite_strdup(error->said));
    return granite_refuse_said(error->kind, error->said, (const char *const *)error->reasons, error->reason_count);
}

void granite_error_free(granite_error *error) {
    if (!error) return;
    for (size_t i = 0; i < error->reason_count; i++) free(error->reasons[i]);
    free(error->reasons);
    free(error->said);
    free(error);
}

granite_error *granite_host_no_model(const char *word) {
    return granite_refuse(GRANITE_REFUSAL_MODEL, "%s asks the host's model, and this host has none loaded", word);
}

granite_error *granite_host_no_answer(const char *fmt, ...) {
    granite_buf reason = {0};
    granite_buf_append_str(&reason, "the host's model gave no answer: ");
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&reason, fmt, args);
    va_end(args);
    return refuse_one(GRANITE_REFUSAL_MODEL, granite_buf_take(&reason));
}

granite_error *granite_host_bad_answer(const char *fmt, ...) {
    granite_buf reason = {0};
    granite_buf_append_str(&reason, "the host's model answered something that does not decode: ");
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&reason, fmt, args);
    va_end(args);
    return refuse_one(GRANITE_REFUSAL_MODEL, granite_buf_take(&reason));
}
