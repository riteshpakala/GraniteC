/* A folder of installed runners. See granite_host/shelf.h. */
#include "granite_host/shelf.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "granite_host/files.h"
#include "granite_host/sha256.h"
#include "granitec/buf.h"

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in `folder`, sorted; with `hidden` false, those starting with a dot left out. */
static char **names_in(const char *folder, bool hidden, size_t *count) {
    *count = 0;
    DIR *dir = opendir(folder);
    if (!dir) return NULL;
    size_t capacity = 16;
    char **names = granite_malloc(capacity * sizeof *names);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!hidden && entry->d_name[0] == '.') continue;
        if (*count == capacity) names = granite_realloc(names, (capacity *= 2) * sizeof *names);
        names[(*count)++] = granite_strdup(entry->d_name);
    }
    closedir(dir);
    qsort(names, *count, sizeof *names, compare_names);
    return names;
}

static void push(char ***list, size_t *count, char *item) {
    *list = granite_realloc(*list, (*count + 1) * sizeof **list);
    (*list)[(*count)++] = item;
}

granite_shelf_read granite_shelf_read_all(const char *folder) {
    granite_shelf_read read = {0};
    size_t count = 0;
    char **names = names_in(folder, false, &count);
    for (size_t n = 0; n < count; n++) {
        char *inside = granite_path_join(folder, names[n]);
        char *manifest = granite_path_join(inside, GRANITE_INSTALLATION_FILE);
        size_t len = 0;
        char *bytes = granite_file_read(manifest, &len);
        free(manifest);
        free(inside);
        if (!bytes) {
            free(names[n]);
            continue;
        }
        json_object *value = NULL;
        char *why = NULL;
        granite_installation *installation = NULL;
        if (granite_json_read(bytes, len, &value, &why)) installation = granite_installation_read(value, &why);
        granite_json_put(value);
        free(bytes);
        if (!installation) {
            push(&read.problems, &read.problem_count,
                 granite_strdupf("%s: its %s does not read: %s", names[n], GRANITE_INSTALLATION_FILE, why));
        } else if (strncmp(installation->format, GRANITE_INSTALLATION_FORMAT, strlen(GRANITE_INSTALLATION_FORMAT)) != 0) {
            push(&read.problems, &read.problem_count,
                 granite_strdupf("%s: %s is not an installation this host reads", names[n], installation->format));
            granite_installation_free(installation);
        } else {
            read.installations = granite_realloc(read.installations, (read.count + 1) * sizeof *read.installations);
            read.installations[read.count++] = installation;
        }
        free(why);
        free(names[n]);
    }
    free(names);
    return read;
}

void granite_shelf_read_free(granite_shelf_read *read) {
    for (size_t i = 0; i < read->count; i++) granite_installation_free(read->installations[i]);
    for (size_t i = 0; i < read->problem_count; i++) free(read->problems[i]);
    free(read->installations);
    free(read->problems);
    memset(read, 0, sizeof *read);
}

static bool same_in_any_case(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

granite_installation *granite_shelf_find(const char *folder, const char *name) {
    granite_shelf_read read = granite_shelf_read_all(folder);
    granite_installation *found = NULL;
    for (size_t i = 0; i < read.count && !found; i++)
        if (same_in_any_case(read.installations[i]->name, name)) found = read.installations[i];
    for (size_t i = 0; i < read.count && !found; i++) {
        json_object *entries = read.installations[i]->entries;
        for (size_t e = 0; e < json_object_array_length(entries) && !found; e++) {
            const char *word = granite_json_get_string(json_object_array_get_idx(entries, e), "word");
            if (word && same_in_any_case(word, name)) found = read.installations[i];
        }
    }
    for (size_t i = 0; i < read.count; i++) {
        if (read.installations[i] == found) read.installations[i] = NULL;
        granite_installation_free(read.installations[i]);
    }
    read.count = 0;
    granite_shelf_read_free(&read);
    return found;
}

char *granite_shelf_folder_of(const char *folder, const granite_installation *installation) {
    return granite_path_join(folder, installation->name);
}

char *granite_shelf_executable_of(const char *folder, const granite_installation *installation) {
    char *inside = granite_shelf_folder_of(folder, installation);
    char *executable = granite_path_join(inside, installation->executable);
    free(inside);
    return executable;
}

char *granite_shelf_stamp(const char *folder) {
    size_t count = 0;
    char **names = names_in(folder, true, &count);
    granite_buf stamp = {0};
    for (size_t n = 0; n < count; n++) {
        char *inside = granite_path_join(folder, names[n]);
        char *manifest = granite_path_join(inside, GRANITE_INSTALLATION_FILE);
        double modified;
        if (granite_stamp_of(manifest, NULL, &modified))
            granite_buf_appendf(&stamp, "%s%s@%.6f", stamp.len ? "," : "", names[n], modified);
        free(manifest);
        free(inside);
        free(names[n]);
    }
    free(names);
    return granite_buf_take(&stamp);
}

granite_remote_error *granite_shelf_verify(const char *folder, const granite_installation *installation) {
    char *path = granite_shelf_executable_of(folder, installation);
    uint64_t size;
    double modified;
    bool same = false;
    if (granite_stamp_of(path, &size, &modified)) {
        if (size == installation->size && installation->has_modified && fabs(modified - installation->modified) < 0.001) {
            same = true;
        } else {
            char digest[65];
            same = granite_sha256_file(path, digest) && strcmp(digest, installation->sha256) == 0;
        }
    }
    if (same) {
        free(path);
        return NULL;
    }
    granite_remote_error *changed = granite_remote_new(GRANITE_REMOTE_CHANGED, granite_installation_word(installation));
    changed->path = path;
    return changed;
}
