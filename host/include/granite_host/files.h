/* Files, as the host side needs them: whole reads and atomic writes, folders made and removed,
 * a file's size and modification time, and time as ISO 8601. The granite command's own; never
 * installed. */
#ifndef GRANITE_HOST_FILES_H
#define GRANITE_HOST_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A file's bytes (the caller frees them; NUL-terminated past `len`), or NULL. */
char *granite_file_read(const char *path, size_t *len);
/* Writes `bytes` to `path` through a sibling temporary and a rename: whole or not at all. */
bool granite_file_write(const char *path, const void *bytes, size_t len);
/* Makes `path` and its parents (0755); true when it is there after. */
bool granite_mkdirs(const char *path);
/* Removes `path` and everything under it; true when it is gone after. */
bool granite_remove_tree(const char *path);
bool granite_exists(const char *path);
bool granite_is_directory(const char *path);
/* A file's size and modification time in seconds since 1970; false when it is not there. */
bool granite_stamp_of(const char *path, uint64_t *size, double *modified);
/* `a/b`, with no doubled slash. The caller frees it. */
char *granite_path_join(const char *a, const char *b);
/* A temporary folder (the caller removes it), under $TMPDIR or /tmp, named for `what`. */
char *granite_temporary_folder(const char *what);
/* Now, in UTC to the second: 2026-11-05T18:00:00Z. */
void granite_now_iso8601(char out[21]);

#endif
