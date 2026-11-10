/* The JSON Schema behind `granite check`. See granite_jsonschema/validate.h. */
#include "granite_jsonschema/validate.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "granite_host/files.h"
#include "granitec/buf.h"

const char *const granite_jsonschema_keywords[] = {
    "$ref", "$defs", "$id", "type", "properties", "required", "items", "additionalProperties", "oneOf", "allOf", "if",
    "then", "else", "enum", "const", "pattern", "minimum",
    /* annotations: they say nothing to a validator */
    "$schema", "title", "description", "examples", "format", NULL,
};

typedef struct registered {
    char *name;
    json_object *document; /* shared: a document registered twice is held twice */
} registered;

struct granite_schemas {
    registered *items;
    size_t count;
};

granite_schemas *granite_schemas_new(void) {
    return granite_calloc(1, sizeof(granite_schemas));
}

void granite_schemas_free(granite_schemas *schemas) {
    if (!schemas) return;
    for (size_t i = 0; i < schemas->count; i++) {
        free(schemas->items[i].name);
        granite_json_put(schemas->items[i].document);
    }
    free(schemas->items);
    free(schemas);
}

static void put(granite_schemas *schemas, const char *name, json_object *document) {
    for (size_t i = 0; i < schemas->count; i++) {
        if (strcmp(schemas->items[i].name, name) == 0) {
            granite_json_put(schemas->items[i].document);
            schemas->items[i].document = document;
            return;
        }
    }
    schemas->items = granite_realloc(schemas->items, (schemas->count + 1) * sizeof *schemas->items);
    schemas->items[schemas->count++] = (registered){granite_strdup(name), document};
}

void granite_schemas_add(granite_schemas *schemas, const char *name, json_object *document) {
    const char *id = granite_json_get_string(document, "$id");
    if (id) put(schemas, id, json_object_get(document));
    put(schemas, name, document);
}

bool granite_schemas_add_folder(granite_schemas *schemas, const char *folder, char **error) {
    DIR *dir = opendir(folder);
    if (!dir) {
        *error = granite_strdupf("%s: %s", folder, strerror(errno));
        return false;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcmp(entry->d_name + n - 5, ".json") != 0) continue;
        char *path = granite_path_join(folder, entry->d_name);
        size_t len = 0;
        char *bytes = granite_file_read(path, &len);
        free(path);
        json_object *document = NULL;
        if (bytes && granite_json_read(bytes, len, &document, NULL) && granite_json_get_string(document, "$id"))
            granite_schemas_add(schemas, entry->d_name, document);
        else
            granite_json_put(document);
        free(bytes);
    }
    closedir(dir);
    return true;
}

const json_object *granite_schemas_find(const granite_schemas *schemas, const char *name) {
    for (size_t i = 0; i < schemas->count; i++)
        if (strcmp(schemas->items[i].name, name) == 0) return schemas->items[i].document;
    return NULL;
}

/* --- validating ---------------------------------------------------------------------------- */

typedef struct problems {
    char **items;
    size_t count;
} problems;

typedef struct validation {
    const granite_schemas *schemas;
    problems *found;
} validation;

static void say(problems *p, granite_buf *path, char *sentence) {
    p->items = granite_realloc(p->items, (p->count + 1) * sizeof *p->items);
    p->items[p->count++] = granite_strdupf("%s at %s", sentence, granite_buf_str(path));
    free(sentence);
}

static char *shown(const json_object *value) {
    return granite_json_write(value, NULL);
}

static const char *type_of(const json_object *value) {
    switch (json_object_get_type((json_object *)value)) {
    case json_type_null: return "null";
    case json_type_boolean: return "boolean";
    case json_type_int: return "integer";
    case json_type_double: return "number";
    case json_type_string: return "string";
    case json_type_array: return "array";
    case json_type_object: return "object";
    }
    return "null";
}

static bool is_type(const json_object *value, const char *type) {
    const char *actual = type_of(value);
    if (strcmp(type, actual) == 0) return true;
    if (strcmp(type, "number") == 0) return strcmp(actual, "integer") == 0;
    if (strcmp(type, "integer") == 0 && strcmp(actual, "number") == 0) {
        double d = json_object_get_double((json_object *)value);
        return isfinite(d) && d == trunc(d);
    }
    return false;
}

static void push_segment(granite_buf *path, const char *segment) {
    granite_buf_append_char(path, '/');
    for (const char *c = segment; *c; c++) {
        if (*c == '~') granite_buf_append_str(path, "~0");
        else if (*c == '/') granite_buf_append_str(path, "~1");
        else granite_buf_append_char(path, *c);
    }
}

/* The node `pointer` (#/…, without the #) names in `document`, or NULL. */
static const json_object *follow_pointer(const json_object *document, const char *pointer) {
    const json_object *at = document;
    if (!*pointer) return at;
    if (*pointer != '/') return NULL;
    char *copy = granite_strdup(pointer + 1);
    char *save = NULL;
    for (char *token = strtok_r(copy, "/", &save); token && at; token = strtok_r(NULL, "/", &save)) {
        granite_buf segment = {0};
        for (char *c = token; *c; c++) {
            if (c[0] == '~' && c[1] == '0') granite_buf_append_char(&segment, '~'), c++;
            else if (c[0] == '~' && c[1] == '1') granite_buf_append_char(&segment, '/'), c++;
            else granite_buf_append_char(&segment, *c);
        }
        if (json_object_get_type((json_object *)at) == json_type_array) {
            char *end = NULL;
            long index = strtol(granite_buf_str(&segment), &end, 10);
            at = *end || index < 0 ? NULL : json_object_array_get_idx((json_object *)at, (size_t)index);
        } else {
            at = granite_json_get(at, granite_buf_str(&segment));
        }
        granite_buf_free(&segment);
    }
    free(copy);
    return at;
}

static void validate(validation *v, const json_object *schema, const json_object *document, const json_object *instance,
                     granite_buf *path, problems *found);

/* Whether `instance` keeps `schema`, saying nothing. */
static bool keeps(validation *v, const json_object *schema, const json_object *document, const json_object *instance) {
    problems quiet = {0};
    granite_buf path = {0};
    validate(v, schema, document, instance, &path, &quiet);
    bool ok = quiet.count == 0;
    for (size_t i = 0; i < quiet.count; i++) free(quiet.items[i]);
    free(quiet.items);
    granite_buf_free(&path);
    return ok;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static const char **sorted_keys(const json_object *object, size_t *count) {
    size_t n = (size_t)json_object_object_length((json_object *)object), i = 0;
    const char **keys = granite_malloc((n ? n : 1) * sizeof *keys);
    json_object_object_foreach((json_object *)object, key, unused) {
        (void)unused;
        keys[i++] = key;
    }
    qsort(keys, i, sizeof *keys, compare_names);
    *count = i;
    return keys;
}

static int compare_type_names(const void *a, const void *b) {
    return strcmp(json_object_get_string(*(json_object *const *)a), json_object_get_string(*(json_object *const *)b));
}

static void check_type(const json_object *types, const json_object *instance, granite_buf *path, problems *found) {
    if (json_object_get_type((json_object *)types) == json_type_string) {
        const char *type = json_object_get_string((json_object *)types);
        if (is_type(instance, type)) return;
        char *value = shown(instance);
        say(found, path, granite_strdupf("%s is not of type \"%s\"", value, type));
        free(value);
        return;
    }
    size_t n = json_object_array_length((json_object *)types);
    for (size_t i = 0; i < n; i++)
        if (is_type(instance, json_object_get_string(json_object_array_get_idx((json_object *)types, i)))) return;
    json_object **names = granite_malloc((n ? n : 1) * sizeof *names);
    for (size_t i = 0; i < n; i++) names[i] = json_object_array_get_idx((json_object *)types, i);
    qsort(names, n, sizeof *names, compare_type_names);
    granite_buf listed = {0};
    for (size_t i = 0; i < n; i++) granite_buf_appendf(&listed, "%s\"%s\"", i ? ", " : "", json_object_get_string(names[i]));
    char *value = shown(instance);
    say(found, path, granite_strdupf("%s is not of types %s", value, granite_buf_str(&listed)));
    free(value);
    free(names);
    granite_buf_free(&listed);
}

static void check_pattern(const char *pattern, const json_object *instance, granite_buf *path, problems *found) {
    if (json_object_get_type((json_object *)instance) != json_type_string) return;
    regex_t compiled;
    if (regcomp(&compiled, pattern, REG_EXTENDED | REG_NOSUB) != 0) return;
    const char *text = json_object_get_string((json_object *)instance);
#ifdef REG_STARTEND
    regmatch_t whole = {0, (regoff_t)json_object_get_string_len((json_object *)instance)};
    int matched = regexec(&compiled, text, 1, &whole, REG_STARTEND);
#else
    int matched = regexec(&compiled, text, 0, NULL, 0);
#endif
    regfree(&compiled);
    if (matched == 0) return;
    char *value = shown(instance);
    say(found, path, granite_strdupf("%s does not match \"%s\"", value, pattern));
    free(value);
}

static void check_properties(validation *v, const json_object *properties, const json_object *document,
                             const json_object *instance, granite_buf *path, problems *found) {
    if (json_object_get_type((json_object *)instance) != json_type_object) return;
    size_t n;
    const char **names = sorted_keys(properties, &n);
    for (size_t i = 0; i < n; i++) {
        if (!granite_json_has(instance, names[i])) continue;
        size_t mark = path->len;
        push_segment(path, names[i]);
        validate(v, granite_json_get(properties, names[i]), document, granite_json_get(instance, names[i]), path, found);
        path->len = mark;
        if (path->data) path->data[mark] = '\0';
    }
    free(names);
}

static void check_additional(validation *v, const json_object *schema, const json_object *additional,
                             const json_object *document, const json_object *instance, granite_buf *path, problems *found) {
    if (json_object_get_type((json_object *)instance) != json_type_object) return;
    const json_object *properties = granite_json_get(schema, "properties");
    size_t n;
    const char **names = sorted_keys(instance, &n);
    granite_buf unexpected = {0};
    size_t extra = 0;
    for (size_t i = 0; i < n; i++) {
        if (granite_json_has(properties, names[i])) continue;
        if (json_object_get_type((json_object *)additional) == json_type_boolean) {
            if (!json_object_get_boolean((json_object *)additional)) {
                granite_buf_appendf(&unexpected, "%s'%s'", extra ? ", " : "", names[i]);
                extra++;
            }
            continue;
        }
        size_t mark = path->len;
        push_segment(path, names[i]);
        validate(v, additional, document, granite_json_get(instance, names[i]), path, found);
        path->len = mark;
        if (path->data) path->data[mark] = '\0';
    }
    if (extra)
        say(found, path, granite_strdupf("Additional properties are not allowed (%s %s unexpected)", granite_buf_str(&unexpected),
                                         extra == 1 ? "was" : "were"));
    granite_buf_free(&unexpected);
    free(names);
}

static void check_ref(validation *v, const char *ref, const json_object *document, const json_object *instance,
                      granite_buf *path, problems *found) {
    const json_object *target_document = document;
    const char *fragment = ref;
    char *uri = NULL;
    if (ref[0] != '#') {
        const char *hash = strchr(ref, '#');
        uri = hash ? granite_strdupf("%.*s", (int)(hash - ref), ref) : granite_strdup(ref);
        fragment = hash ? hash : "#";
        target_document = granite_schemas_find(v->schemas, uri);
    }
    const json_object *target = target_document ? follow_pointer(target_document, fragment + 1) : NULL;
    if (target) validate(v, target, target_document, instance, path, found);
    else say(found, path, granite_strdupf("unresolvable reference %s", ref));
    free(uri);
}

static void validate(validation *v, const json_object *schema, const json_object *document, const json_object *instance,
                     granite_buf *path, problems *found) {
    if (json_object_get_type((json_object *)schema) == json_type_boolean) {
        if (!json_object_get_boolean((json_object *)schema)) {
            char *value = shown(instance);
            say(found, path, granite_strdupf("False schema does not allow %s", value));
            free(value);
        }
        return;
    }
    if (json_object_get_type((json_object *)schema) != json_type_object) return;
    size_t n;
    const char **keywords = sorted_keys(schema, &n);
    for (size_t k = 0; k < n; k++) {
        const char *keyword = keywords[k];
        const json_object *value = granite_json_get(schema, keyword);
        if (strcmp(keyword, "$ref") == 0) {
            check_ref(v, json_object_get_string((json_object *)value), document, instance, path, found);
        } else if (strcmp(keyword, "type") == 0) {
            check_type(value, instance, path, found);
        } else if (strcmp(keyword, "properties") == 0) {
            check_properties(v, value, document, instance, path, found);
        } else if (strcmp(keyword, "additionalProperties") == 0) {
            check_additional(v, schema, value, document, instance, path, found);
        } else if (strcmp(keyword, "required") == 0) {
            if (json_object_get_type((json_object *)instance) != json_type_object) continue;
            for (size_t i = 0; i < json_object_array_length((json_object *)value); i++) {
                const char *name = json_object_get_string(json_object_array_get_idx((json_object *)value, i));
                if (granite_json_has(instance, name)) continue;
                granite_buf quoted = {0};
                granite_json_append_string(&quoted, name, strlen(name));
                say(found, path, granite_strdupf("%s is a required property", granite_buf_str(&quoted)));
                granite_buf_free(&quoted);
            }
        } else if (strcmp(keyword, "items") == 0) {
            if (json_object_get_type((json_object *)instance) != json_type_array) continue;
            for (size_t i = 0; i < json_object_array_length((json_object *)instance); i++) {
                size_t mark = path->len;
                granite_buf_appendf(path, "/%zu", i);
                validate(v, value, document, json_object_array_get_idx((json_object *)instance, i), path, found);
                path->len = mark;
                if (path->data) path->data[mark] = '\0';
            }
        } else if (strcmp(keyword, "allOf") == 0) {
            for (size_t i = 0; i < json_object_array_length((json_object *)value); i++)
                validate(v, json_object_array_get_idx((json_object *)value, i), document, instance, path, found);
        } else if (strcmp(keyword, "oneOf") == 0) {
            size_t matched = 0;
            for (size_t i = 0; i < json_object_array_length((json_object *)value); i++)
                if (keeps(v, json_object_array_get_idx((json_object *)value, i), document, instance)) matched++;
            if (matched != 1) {
                char *shown_instance = shown(instance);
                say(found, path,
                    granite_strdupf(matched ? "%s is valid under more than one of the schemas listed in the 'oneOf' keyword"
                                            : "%s is not valid under any of the schemas listed in the 'oneOf' keyword",
                                    shown_instance));
                free(shown_instance);
            }
        } else if (strcmp(keyword, "if") == 0) {
            const json_object *then = granite_json_get(schema, "then"), *otherwise = granite_json_get(schema, "else");
            if (keeps(v, value, document, instance)) {
                if (then) validate(v, then, document, instance, path, found);
            } else if (otherwise) {
                validate(v, otherwise, document, instance, path, found);
            }
        } else if (strcmp(keyword, "enum") == 0) {
            bool listed = false;
            for (size_t i = 0; i < json_object_array_length((json_object *)value) && !listed; i++)
                listed = granite_json_equal(json_object_array_get_idx((json_object *)value, i), instance);
            if (!listed) {
                char *shown_instance = shown(instance), *options = shown(value);
                say(found, path, granite_strdupf("%s is not one of %s", shown_instance, options));
                free(shown_instance);
                free(options);
            }
        } else if (strcmp(keyword, "const") == 0) {
            if (!granite_json_equal(value, instance)) {
                char *expected = shown(value);
                say(found, path, granite_strdupf("%s was expected", expected));
                free(expected);
            }
        } else if (strcmp(keyword, "pattern") == 0) {
            check_pattern(json_object_get_string((json_object *)value), instance, path, found);
        } else if (strcmp(keyword, "minimum") == 0) {
            if (granite_json_is_number(instance) &&
                json_object_get_double((json_object *)instance) < json_object_get_double((json_object *)value)) {
                char *shown_instance = shown(instance), *limit = shown(value);
                say(found, path, granite_strdupf("%s is less than the minimum of %s", shown_instance, limit));
                free(shown_instance);
                free(limit);
            }
        }
        /* $defs, $id, then, else and the annotations say nothing by themselves. */
    }
    free(keywords);
}

char **granite_schema_validate(const granite_schemas *schemas, const json_object *schema, const json_object *instance,
                               size_t *count) {
    problems found = {0};
    validation v = {schemas, &found};
    granite_buf path = {0};
    validate(&v, schema, schema, instance, &path, &found);
    granite_buf_free(&path);
    *count = found.count;
    return found.items;
}
