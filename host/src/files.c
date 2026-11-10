/* Files, as the host side needs them. See granite_host/files.h. */
#include "granite_host/files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "granitec/buf.h"

char *granite_file_read(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    granite_buf bytes = {0};
    char chunk[65536];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            close(fd);
            granite_buf_free(&bytes);
            return NULL;
        }
        if (n == 0) break;
        granite_buf_append(&bytes, chunk, (size_t)n);
    }
    close(fd);
    if (len) *len = bytes.len;
    return granite_buf_take(&bytes);
}

bool granite_file_write(const char *path, const void *bytes, size_t len) {
    char *temporary = granite_strdupf("%s.%d.writing", path, (int)getpid());
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(temporary);
        return false;
    }
    const char *at = bytes;
    size_t left = len;
    bool ok = true;
    while (left && ok) {
        ssize_t n = write(fd, at, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else at += n, left -= (size_t)n;
    }
    ok = close(fd) == 0 && ok;
    ok = ok && rename(temporary, path) == 0;
    if (!ok) unlink(temporary);
    free(temporary);
    return ok;
}

bool granite_mkdirs(const char *path) {
    char *copy = granite_strdup(path);
    for (char *slash = copy + 1; *slash; slash++) {
        if (*slash != '/') continue;
        *slash = '\0';
        mkdir(copy, 0755);
        *slash = '/';
    }
    mkdir(copy, 0755);
    bool made = granite_is_directory(copy);
    free(copy);
    return made;
}

bool granite_exists(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0;
}

bool granite_is_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool granite_remove_tree(const char *path) {
    struct stat info;
    if (lstat(path, &info) != 0) return errno == ENOENT;
    if (S_ISDIR(info.st_mode)) {
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                char *child = granite_path_join(path, entry->d_name);
                granite_remove_tree(child);
                free(child);
            }
            closedir(dir);
        }
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

bool granite_stamp_of(const char *path, uint64_t *size, double *modified) {
    struct stat info;
    if (stat(path, &info) != 0) return false;
    if (size) *size = (uint64_t)info.st_size;
#if defined(__APPLE__)
    struct timespec mtime = info.st_mtimespec;
#else
    struct timespec mtime = info.st_mtim;
#endif
    if (modified) *modified = (double)mtime.tv_sec + (double)mtime.tv_nsec / 1e9;
    return true;
}

char *granite_path_join(const char *a, const char *b) {
    size_t n = strlen(a);
    while (n > 1 && a[n - 1] == '/') n--;
    while (*b == '/') b++;
    return granite_strdupf("%.*s/%s", (int)n, a, b);
}

char *granite_temporary_folder(const char *what) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/') n--; /* macOS's TMPDIR ends with one */
    char *pattern = granite_strdupf("%.*s/granitec-%s-XXXXXX", (int)n, base, what);
    for (char *c = pattern + n + 1; *c; c++)
        if (*c == '/') *c = '-';
    char *made = mkdtemp(pattern);
    if (!made) {
        free(pattern);
        return NULL;
    }
    return pattern;
}

void granite_now_iso8601(char out[21]) {
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &utc);
}
