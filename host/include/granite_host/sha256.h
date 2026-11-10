/* SHA-256 (FIPS 180-4), with no library behind it: MaryOS's common/sha256 under Granite's names.
 * What an installation records of its executable, and what the shelf checks before a call. */
#ifndef GRANITE_HOST_SHA256_H
#define GRANITE_HOST_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define GRANITE_SHA256_BYTES 32

typedef struct granite_sha256_ctx {
    uint32_t state[8];
    uint64_t length;            /* bytes so far */
    unsigned char block[64];
    size_t block_len;
} granite_sha256_ctx;

void granite_sha256_init(granite_sha256_ctx *c);
void granite_sha256_update(granite_sha256_ctx *c, const void *data, size_t len);
void granite_sha256_final(granite_sha256_ctx *c, unsigned char out[GRANITE_SHA256_BYTES]);
/* One shot. */
void granite_sha256(const void *data, size_t len, unsigned char out[GRANITE_SHA256_BYTES]);
/* Lowercase hex, NUL-terminated (65 bytes). */
void granite_sha256_hex(const unsigned char digest[GRANITE_SHA256_BYTES], char out[65]);

/* A file's digest, as lowercase hex in `out`; false when it could not be read. */
#include <stdbool.h>
bool granite_sha256_file(const char *path, char out[65]);

#endif
