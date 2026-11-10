/* A growable run of bytes, kept NUL-terminated: what every writer in GraniteC writes into. */
#ifndef GRANITEC_BUF_H
#define GRANITEC_BUF_H

#include <stdarg.h>
#include <stddef.h>

typedef struct granite_buf {
    char *data;
    size_t len;
    size_t cap;
} granite_buf;

void granite_buf_append(granite_buf *buf, const void *bytes, size_t n);
void granite_buf_append_str(granite_buf *buf, const char *text);
void granite_buf_append_char(granite_buf *buf, char c);
void granite_buf_appendf(granite_buf *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void granite_buf_vappendf(granite_buf *buf, const char *fmt, va_list args);
/* The text so far ("" when nothing was written); the buffer keeps it. */
const char *granite_buf_str(const granite_buf *buf);
/* The text, handed over (the caller frees it); the buffer is empty after. Never NULL. */
char *granite_buf_take(granite_buf *buf);
void granite_buf_reset(granite_buf *buf);
void granite_buf_free(granite_buf *buf);

/* strdup that aborts instead of returning NULL; NULL in, NULL out. */
char *granite_strdup(const char *text);
/* A formatted string the caller frees. */
char *granite_strdupf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* malloc, calloc and realloc that abort when memory runs out. */
void *granite_malloc(size_t n);
void *granite_calloc(size_t count, size_t n);
void *granite_realloc(void *p, size_t n);

#endif
