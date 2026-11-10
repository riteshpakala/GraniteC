/* granite-answer/1, built, checked and stamped. See granitec/answer.h.
 *
 * The check is GraniteRust's AnswerCheck (Swift's AgentAnswerCheck), rule for rule and sentence for
 * sentence, in the same order. */
#include "granitec/answer.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "granitec/buf.h"

static const char *const STAMPED[] = {"format", "agent"};
static const char *const STATES[] = {"written", "rewritten", "unchanged", "removed"};

static json_object *list_of(json_object *answer, const char *key) {
    json_object *list = granite_json_get(answer, key);
    if (json_object_get_type(list) != json_type_array) {
        list = json_object_new_array();
        granite_json_set(answer, key, list);
    }
    return list;
}

json_object *granite_answer_new(void) {
    json_object *answer = json_object_new_object();
    granite_json_set(answer, "path", NULL);
    static const char *const lists[] = {"paths", "made", "expected", "problems", "payloads"};
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) granite_json_set(answer, lists[i], json_object_new_array());
    return answer;
}

void granite_answer_path(json_object *answer, const char *path) {
    granite_json_set_string(answer, "path", path);
    granite_answer_add_path(answer, path);
}

void granite_answer_add_path(json_object *answer, const char *path) {
    json_object_array_add(list_of(answer, "paths"), json_object_new_string(path));
}

void granite_answer_note(json_object *answer, const char *fmt, ...) {
    granite_buf note = {0};
    va_list args;
    va_start(args, fmt);
    granite_buf_vappendf(&note, fmt, args);
    va_end(args);
    json_object_array_add(list_of(answer, "notes"), json_object_new_string(granite_buf_str(&note)));
    granite_buf_free(&note);
}

void granite_answer_made(json_object *answer, const char *kind, const char *name, const char *path,
                         const char *state) {
    json_object *made = json_object_new_object();
    granite_json_set_string(made, "kind", kind);
    granite_json_set_string(made, "name", name);
    granite_json_set_string(made, "path", path);
    granite_json_set_string(made, "state", state ? state : "written");
    json_object_array_add(list_of(answer, "made"), made);
}

static void pair(json_object *answer, const char *list, const char *first_key, const char *first,
                 const char *second_key, const char *second) {
    json_object *item = json_object_new_object();
    granite_json_set_string(item, first_key, first);
    granite_json_set_string(item, second_key, second);
    json_object_array_add(list_of(answer, list), item);
}

void granite_answer_expected(json_object *answer, const char *kind, const char *name) {
    pair(answer, "expected", "kind", kind, "name", name);
}

void granite_answer_problem(json_object *answer, const char *kind, const char *text) {
    pair(answer, "problems", "kind", kind, "text", text);
}

void granite_answer_payload(json_object *answer, const char *path, const char *schema) {
    pair(answer, "payloads", "path", path, "schema", schema);
}

/* --- paths as files ------------------------------------------------------------------------ */

char *granite_answer_resolved(const char *path, const char *directory) {
    granite_buf joined = {0};
    if (path[0] == '/') {
        granite_buf_append_str(&joined, path);
    } else {
        char here[PATH_MAX];
        const char *base = directory ? directory : (getcwd(here, sizeof here) ? here : "/");
        granite_buf_appendf(&joined, "%s/%s", base, path);
    }
    /* Lexically: "/" then each normal part, `..` popping one, `.` and empty parts skipped. */
    granite_buf lexical = {0};
    char *parts = granite_buf_take(&joined);
    for (char *save = NULL, *part = strtok_r(parts, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            char *slash = lexical.len ? strrchr(lexical.data, '/') : NULL;
            if (slash) {
                lexical.len = (size_t)(slash - lexical.data);
                lexical.data[lexical.len] = '\0';
            }
            continue;
        }
        granite_buf_appendf(&lexical, "/%s", part);
    }
    free(parts);
    if (!lexical.len) granite_buf_append_char(&lexical, '/');
    /* The longest ancestor that exists, resolved, and the rest after it. */
    char *whole = granite_buf_take(&lexical);
    size_t cut = strlen(whole);
    for (;;) {
        char prefix[PATH_MAX], real[PATH_MAX];
        snprintf(prefix, sizeof prefix, "%.*s", (int)(cut ? cut : 1), cut ? whole : "/");
        if (realpath(prefix, real)) {
            const char *rest = whole + cut;
            char *out = strcmp(real, "/") == 0 ? granite_strdup(*rest ? rest : "/") : granite_strdupf("%s%s", real, rest);
            free(whole);
            return out;
        }
        if (cut == 0) return whole;
        size_t i = cut;
        while (i > 0 && whole[i - 1] != '/') i--;
        cut = i > 0 ? i - 1 : 0;
    }
}

/* --- the check ----------------------------------------------------------------------------- */

typedef struct said {
    char **items;
    size_t count, capacity;
} said;

static void say(said *s, char *sentence) {
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 8;
        s->items = granite_realloc(s->items, s->capacity * sizeof *s->items);
    }
    s->items[s->count++] = sentence;
}

/* The resolved `paths`: the set a path must be in. */
typedef struct path_set {
    char **items;
    size_t count;
} path_set;

static bool contains(const path_set *set, const char *path) {
    for (size_t i = 0; i < set->count; i++)
        if (strcmp(set->items[i], path) == 0) return true;
    return false;
}

static bool in_paths(const path_set *set, const char *path, const char *directory) {
    char *resolved = granite_answer_resolved(path, directory);
    bool found = contains(set, resolved);
    free(resolved);
    return found;
}

static const char *string_of(const json_object *object, const char *key) {
    return granite_json_get_string(object, key);
}

static bool lowercase_word(const char *text) {
    if (!(*text >= 'a' && *text <= 'z')) return false;
    for (const char *c = text + 1; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return false;
    return true;
}

static bool makes_kind(const granite_make *makes, const char *kind) {
    for (const granite_make *m = makes; m && m->kind; m++)
        if (strcmp(m->kind, kind) == 0) return true;
    return false;
}

static char *declared_of(const granite_make *makes) {
    if (!makes || !makes->kind) return granite_strdup("declares nothing it makes (`makes`)");
    granite_buf text = {0};
    granite_buf_append_str(&text, "makes ");
    for (const granite_make *m = makes; m->kind; m++) granite_buf_appendf(&text, "%s`%s`", m == makes ? "" : ", ", m->kind);
    return granite_buf_take(&text);
}

typedef struct check {
    said problems;
    path_set paths;
    const char *word;
    const granite_make *makes;
    const char *directory;
    char *declared;
} check;

static void check_made(check *c, const json_object *item, size_t index) {
    const char *kind = string_of(item, "kind"), *name = string_of(item, "name");
    if (!kind || !name || !*name) {
        say(&c->problems, granite_strdupf("`made[%zu]` needs a `kind` and a `name`", index));
        return;
    }
    if (!makes_kind(c->makes, kind))
        say(&c->problems, granite_strdupf("`made[%zu]` is a `%s`, and %s %s", index, kind, c->word, c->declared));
    const char *state_text = string_of(item, "state");
    const char *state = NULL;
    for (size_t i = 0; state_text && i < 4; i++)
        if (strcmp(STATES[i], state_text) == 0) state = STATES[i];
    if (granite_json_has(item, "state") && !state)
        say(&c->problems, granite_strdupf("`made[%zu].state` is one of `written`, `rewritten`, `unchanged`, `removed`", index));
    if (!state || strcmp(state, "written") == 0 || strcmp(state, "rewritten") == 0) {
        const char *path = string_of(item, "path");
        if (!path) say(&c->problems, granite_strdupf("`made[%zu]` was written, so it names its `path`", index));
        else if (!in_paths(&c->paths, path, c->directory))
            say(&c->problems, granite_strdupf("`made[%zu]` is in %s, which is not one of `paths`", index, path));
    }
}

static void check_expected(check *c, const json_object *item, size_t index) {
    const char *kind = string_of(item, "kind");
    if (!kind || !string_of(item, "name")) {
        say(&c->problems, granite_strdupf("`expected[%zu]` needs a `kind` and a `name`", index));
        return;
    }
    if (!makes_kind(c->makes, kind))
        say(&c->problems, granite_strdupf("`expected[%zu]` is a `%s`, and %s %s", index, kind, c->word, c->declared));
}

static void check_problem(check *c, const json_object *item, size_t index) {
    const char *kind = string_of(item, "kind");
    if (!kind || !string_of(item, "text")) {
        say(&c->problems, granite_strdupf("`problems[%zu]` needs a `kind` and a `text`", index));
        return;
    }
    if (!lowercase_word(kind)) say(&c->problems, granite_strdupf("`problems[%zu].kind` is one lowercase word", index));
}

static void check_payload(check *c, const json_object *item, size_t index) {
    const char *path = string_of(item, "path"), *schema = string_of(item, "schema");
    if (!path || !schema) {
        say(&c->problems, granite_strdupf("`payloads[%zu]` needs a `path` and a `schema`", index));
        return;
    }
    char *file = granite_answer_resolved(path, c->directory);
    if (!contains(&c->paths, file)) {
        say(&c->problems, granite_strdupf("`payloads[%zu]` %s is not one of `paths`", index, path));
        free(file);
        return;
    }
    json_object *payload = NULL;
    FILE *opened = fopen(file, "rb");
    if (opened) {
        granite_buf bytes = {0};
        char chunk[16384];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, opened)) > 0) granite_buf_append(&bytes, chunk, n);
        fclose(opened);
        granite_json_read(granite_buf_str(&bytes), bytes.len, &payload, NULL);
        granite_buf_free(&bytes);
    }
    free(file);
    if (json_object_get_type(payload) != json_type_object) {
        say(&c->problems, granite_strdupf("`payloads[%zu]` %s is not a JSON object on disk", index, path));
    } else {
        const char *own = string_of(payload, "schema");
        if (!own)
            say(&c->problems, granite_strdupf("`payloads[%zu]` says `%s`, and the file's own `schema` is missing", index, schema));
        else if (strcmp(own, schema) != 0)
            say(&c->problems, granite_strdupf("`payloads[%zu]` says `%s`, and the file's own `schema` is `%s`", index, schema, own));
    }
    granite_json_put(payload);
}

/* Each item of a reserved list, checked; a key that is not a list of objects is one problem. */
static void each(check *c, const json_object *answer, const char *key,
                 void (*one)(check *, const json_object *, size_t)) {
    if (!granite_json_has(answer, key)) return;
    json_object *list = granite_json_get(answer, key);
    if (json_object_get_type(list) != json_type_array) {
        say(&c->problems, granite_strdupf("`%s` is not a list", key));
        return;
    }
    for (size_t i = 0; i < json_object_array_length(list); i++) {
        json_object *item = json_object_array_get_idx(list, i);
        if (json_object_get_type(item) != json_type_object) say(&c->problems, granite_strdupf("`%s[%zu]` is not an object", key, i));
        else one(c, item, i);
    }
}

char **granite_answer_problems(const json_object *answer, const char *word, const granite_make *makes,
                               const char *directory, size_t *count) {
    check c = {{0}, {0}, word, makes, directory, declared_of(makes)};
    for (size_t i = 0; i < 2; i++)
        if (granite_json_has(answer, STAMPED[i]))
            say(&c.problems, granite_strdupf("`%s` is the runner's to say: an answer does not encode it", STAMPED[i]));

    if (!granite_json_has(answer, "paths")) {
        say(&c.problems, granite_strdup("`paths` is missing: an answer names every file it wrote, `[]` when none"));
    } else {
        json_object *paths = granite_json_get(answer, "paths");
        if (json_object_get_type(paths) != json_type_array) {
            say(&c.problems, granite_strdup("`paths` is not a list of paths"));
        } else {
            size_t n = json_object_array_length(paths);
            c.paths.items = granite_calloc(n ? n : 1, sizeof *c.paths.items);
            for (size_t i = 0; i < n; i++) {
                json_object *item = json_object_array_get_idx(paths, i);
                if (json_object_get_type(item) == json_type_string)
                    c.paths.items[c.paths.count++] = granite_answer_resolved(json_object_get_string(item), directory);
                else
                    say(&c.problems, granite_strdup("`paths` holds something that is not a path"));
            }
        }
    }

    if (granite_json_has(answer, "path")) {
        json_object *path = granite_json_get(answer, "path");
        if (json_object_get_type(path) == json_type_string) {
            if (!in_paths(&c.paths, json_object_get_string(path), directory))
                say(&c.problems, granite_strdupf("`path` %s is not one of `paths`", json_object_get_string(path)));
        } else if (path != NULL) {
            say(&c.problems, granite_strdup("`path` is not a path"));
        }
    }

    each(&c, answer, "made", check_made);
    each(&c, answer, "expected", check_expected);
    each(&c, answer, "problems", check_problem);
    each(&c, answer, "payloads", check_payload);

    if (granite_json_has(answer, "notes")) {
        json_object *notes = granite_json_get(answer, "notes");
        bool sentences = json_object_get_type(notes) == json_type_array;
        for (size_t i = 0; sentences && i < json_object_array_length(notes); i++)
            sentences = json_object_get_type(json_object_array_get_idx(notes, i)) == json_type_string;
        if (!sentences) say(&c.problems, granite_strdup("`notes` is a list of sentences"));
    }

    for (size_t i = 0; i < c.paths.count; i++) free(c.paths.items[i]);
    free(c.paths.items);
    free(c.declared);
    *count = c.problems.count;
    return c.problems.items;
}

json_object *granite_answer_stamped(const json_object *answer, const char *word) {
    json_object *out = granite_json_copy(answer);
    granite_json_set_string(out, "format", GRANITE_ANSWER_FORMAT);
    json_object *agent = json_object_new_object();
    granite_json_set_string(agent, "word", word);
    granite_json_set(out, "agent", agent);
    return out;
}
