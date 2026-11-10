/* A growable run of bytes, and allocation that never returns NULL. See granitec/buf.h. */
#include "granitec/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(size_t n) {
    fprintf(stderr, "granite: out of memory (%zu bytes)\n", n);
    abort();
}

void *granite_malloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory(n);
    return p;
}

void *granite_calloc(size_t count, size_t n) {
    void *p = calloc(count ? count : 1, n ? n : 1);
    if (!p) out_of_memory(count * n);
    return p;
}

void *granite_realloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) out_of_memory(n);
    return q;
}

char *granite_strdup(const char *text) {
    if (!text) return NULL;
    size_t n = strlen(text);
    char *copy = granite_malloc(n + 1);
    memcpy(copy, text, n + 1);
    return copy;
}

char *granite_strdupf(const char *fmt, ...) {
    granite_buf buf = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&buf, fmt, args);
    va_end(args);
    return granite_buf_take(&buf);
}

static void reserve(granite_buf *buf, size_t more) {
    if (buf->len + more + 1 <= buf->cap) return;
    size_t cap = buf->cap ? buf->cap : 64;
    while (cap < buf->len + more + 1) cap *= 2;
    buf->data = granite_realloc(buf->data, cap);
    buf->cap = cap;
}

void granite_buf_append(granite_buf *buf, const void *bytes, size_t n) {
    reserve(buf, n);
    if (n) memcpy(buf->data + buf->len, bytes, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
}

void granite_buf_append_str(granite_buf *buf, const char *text) {
    granite_buf_append(buf, text, strlen(text));
}

void granite_buf_append_char(granite_buf *buf, char c) {
    granite_buf_append(buf, &c, 1);
}

void granite_buf_vappendf(granite_buf *buf, const char *fmt, va_list args) {
    va_list again;
    va_copy(again, args);
    int n = vsnprintf(NULL, 0, fmt, again);
    va_end(again);
    if (n < 0) return;
    reserve(buf, (size_t)n);
    vsnprintf(buf->data + buf->len, (size_t)n + 1, fmt, args);
    buf->len += (size_t)n;
}

void granite_buf_appendf(granite_buf *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(buf, fmt, args);
    va_end(args);
}

const char *granite_buf_str(const granite_buf *buf) {
    return buf->data ? buf->data : "";
}

char *granite_buf_take(granite_buf *buf) {
    char *text = buf->data ? buf->data : granite_strdup("");
    buf->data = NULL;
    buf->len = buf->cap = 0;
    return text;
}

void granite_buf_reset(granite_buf *buf) {
    buf->len = 0;
    if (buf->data) buf->data[0] = '\0';
}

void granite_buf_free(granite_buf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}
