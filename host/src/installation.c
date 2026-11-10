/* One installed runner, and where its code came from. See granite_host/installation.h. */
#include "granite_host/installation.h"

#include <stdlib.h>
#include <string.h>

#include "granitec/buf.h"
#include "granitec/schema.h"

granite_source granite_source_parse(const char *said, const char *base) {
    granite_source source = {0};
    while (*said == ' ' || *said == '\t' || *said == '\n') said++;
    size_t n = strlen(said);
    while (n && (said[n - 1] == ' ' || said[n - 1] == '\t' || said[n - 1] == '\n')) n--;
    char *trimmed = granite_strdupf("%.*s", (int)n, said);
    if (trimmed[0] == '/' || trimmed[0] == '~' || trimmed[0] == '.') {
        char *joined;
        if (trimmed[0] == '~') {
            const char *home = getenv("HOME");
            const char *rest = trimmed + 1;
            while (*rest == '/') rest++;
            joined = granite_strdupf("%s/%s", home ? home : "", rest);
        } else if (trimmed[0] == '/') {
            joined = granite_strdup(trimmed);
        } else {
            joined = granite_strdupf("%s/%s", base ? base : ".", trimmed);
        }
        /* "/" then each normal part, `..` popping one. */
        granite_buf clean = {0};
        char *save = NULL;
        for (char *part = strtok_r(joined, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
            if (strcmp(part, ".") == 0) continue;
            if (strcmp(part, "..") == 0) {
                char *slash = clean.len ? strrchr(clean.data, '/') : NULL;
                if (slash) {
                    clean.len = (size_t)(slash - clean.data);
                    clean.data[clean.len] = '\0';
                }
                continue;
            }
            granite_buf_appendf(&clean, "/%s", part);
        }
        if (!clean.len) granite_buf_append_char(&clean, '/');
        source.path = granite_buf_take(&clean);
        free(joined);
        free(trimmed);
        return source;
    }
    /* A revision follows the last @ after the last /: git@host:x/y.git has none. */
    char *at = strrchr(trimmed, '@'), *slash = strrchr(trimmed, '/');
    if (at && slash && at > slash) {
        source.url = granite_strdupf("%.*s", (int)(at - trimmed), trimmed);
        source.revision = granite_strdup(at + 1);
    } else {
        source.url = granite_strdup(trimmed);
    }
    free(trimmed);
    return source;
}

granite_source granite_source_newest(const granite_source *source) {
    granite_source newest = {granite_strdup(source->path), granite_strdup(source->url), NULL};
    return newest;
}

char *granite_source_text(const granite_source *source) {
    if (source->path) return granite_strdup(source->path);
    if (source->revision) return granite_strdupf("%s@%s", source->url, source->revision);
    return granite_strdup(source->url ? source->url : "");
}

json_object *granite_source_json(const granite_source *source) {
    json_object *json = json_object_new_object();
    if (source->path) {
        granite_json_set_string(json, "path", source->path);
    } else {
        granite_json_set_string(json, "url", source->url ? source->url : "");
        granite_json_set_string(json, "revision", source->revision);
    }
    return json;
}

void granite_source_free(granite_source *source) {
    free(source->path);
    free(source->url);
    free(source->revision);
    memset(source, 0, sizeof *source);
}

json_object *granite_installation_json(const granite_installation *i) {
    json_object *json = json_object_new_object();
    granite_json_set_string(json, "format", i->format ? i->format : GRANITE_INSTALLATION_FORMAT);
    granite_json_set_string(json, "name", i->name);
    granite_json_set(json, "source", granite_source_json(&i->source));
    granite_json_set_string(json, "revision", i->revision);
    granite_json_set_string(json, "granite", i->granite);
    granite_json_set_string(json, "executable", i->executable);
    granite_json_set_string(json, "sha256", i->sha256);
    granite_json_set(json, "size", json_object_new_uint64(i->size));
    if (i->has_modified) granite_json_set(json, "modified", json_object_new_double(i->modified));
    granite_json_set_string(json, "builtAt", i->built_at);
    granite_json_set(json, "entries", i->entries ? granite_json_copy(i->entries) : json_object_new_array());
    return json;
}

/* A string field: copied into `*out`; false with why when it is missing or not a string. */
static bool string_field(const json_object *value, const char *key, bool required, char **out, char **error) {
    if (!granite_json_has(value, key) || (!required && !granite_json_get(value, key))) {
        if (required) *error = granite_strdupf("missing field `%s`", key);
        return !required;
    }
    json_object *member = granite_json_get(value, key);
    if (json_object_get_type(member) != json_type_string) {
        char *unexpected = granite_json_unexpected(member);
        *error = granite_strdupf("invalid type: %s, expected a string", unexpected);
        free(unexpected);
        return false;
    }
    *out = granite_strdup(json_object_get_string(member));
    return true;
}

granite_installation *granite_installation_read(const json_object *value, char **error) {
    *error = NULL;
    if (json_object_get_type((json_object *)value) != json_type_object) {
        char *unexpected = granite_json_unexpected(value);
        *error = granite_strdupf("invalid type: %s, expected struct AgentInstallation", unexpected);
        free(unexpected);
        return NULL;
    }
    granite_installation *i = granite_calloc(1, sizeof *i);
    bool ok = string_field(value, "format", true, &i->format, error) && string_field(value, "name", true, &i->name, error);
    if (ok) {
        json_object *source = granite_json_get(value, "source");
        if (!granite_json_has(value, "source")) {
            *error = granite_strdup("missing field `source`");
            ok = false;
        } else if (json_object_get_type(source) == json_type_object &&
                   json_object_get_type(granite_json_get(source, "path")) == json_type_string) {
            i->source.path = granite_strdup(granite_json_get_string(source, "path"));
        } else if (json_object_get_type(source) == json_type_object &&
                   json_object_get_type(granite_json_get(source, "url")) == json_type_string) {
            i->source.url = granite_strdup(granite_json_get_string(source, "url"));
            i->source.revision = granite_strdup(granite_json_get_string(source, "revision"));
        } else {
            *error = granite_strdup("data did not match any variant of untagged enum AgentSource");
            ok = false;
        }
    }
    ok = ok && string_field(value, "revision", false, &i->revision, error) &&
         string_field(value, "granite", false, &i->granite, error) &&
         string_field(value, "executable", true, &i->executable, error) &&
         string_field(value, "sha256", true, &i->sha256, error);
    if (ok) {
        json_object *size = granite_json_get(value, "size");
        if (!granite_json_has(value, "size")) {
            *error = granite_strdup("missing field `size`");
            ok = false;
        } else if (!granite_json_is_integer(size) || json_object_get_int64(size) < 0) {
            char *unexpected = granite_json_unexpected(size);
            *error = granite_strdupf("invalid type: %s, expected u64", unexpected);
            free(unexpected);
            ok = false;
        } else {
            int64_t signed_size = json_object_get_int64(size);
            i->size = signed_size == INT64_MAX ? json_object_get_uint64(size) : (uint64_t)signed_size;
        }
    }
    if (ok) {
        json_object *modified = granite_json_get(value, "modified");
        if (modified && granite_json_is_number(modified)) {
            i->has_modified = true;
            i->modified = json_object_get_double(modified);
        } else if (modified) {
            char *unexpected = granite_json_unexpected(modified);
            *error = granite_strdupf("invalid type: %s, expected f64", unexpected);
            free(unexpected);
            ok = false;
        }
    }
    ok = ok && string_field(value, "builtAt", true, &i->built_at, error);
    if (ok) {
        json_object *entries = granite_json_get(value, "entries");
        if (!granite_json_has(value, "entries")) {
            *error = granite_strdup("missing field `entries`");
            ok = false;
        } else if (json_object_get_type(entries) != json_type_array) {
            char *unexpected = granite_json_unexpected(entries);
            *error = granite_strdupf("invalid type: %s, expected a sequence", unexpected);
            free(unexpected);
            ok = false;
        } else {
            i->entries = granite_json_copy(entries);
        }
    }
    if (!ok) {
        granite_installation_free(i);
        return NULL;
    }
    return i;
}

const char *granite_installation_word(const granite_installation *i) {
    json_object *first = json_object_array_length(i->entries) ? json_object_array_get_idx(i->entries, 0) : NULL;
    const char *word = granite_json_get_string(first, "word");
    return word ? word : i->name;
}

void granite_installation_free(granite_installation *i) {
    if (!i) return;
    free(i->format);
    free(i->name);
    granite_source_free(&i->source);
    free(i->revision);
    free(i->granite);
    free(i->executable);
    free(i->sha256);
    free(i->built_at);
    granite_json_put(i->entries);
    free(i);
}
