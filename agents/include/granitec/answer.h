/* granite-answer/1: one JSON object whose reserved keys a host reads, beside the package's own.
 *
 *   format    the runner's: granite-answer/1         agent     the runner's: {"word": …}
 *   path      the first file written, or null         paths     every file written or removed; [] when none
 *   notes     what the model should know              made      kind (one of its makes), name, path, state
 *   expected  what the call asked for: kind, name     problems  what it could only do in part: kind, text
 *   payloads  files among paths keeping a standard of their own: path, schema
 *
 * An answer is a json_object. granite_answer_new starts one as Rust's Answer<T> serialises (path
 * null, the lists empty, no notes); the helpers add to it, and the package's own keys are set on
 * it beside them. The runner checks it (granite_answer_problems) and stamps it before it leaves. */
#ifndef GRANITEC_ANSWER_H
#define GRANITEC_ANSWER_H

#include <stdbool.h>
#include <stddef.h>

#include "granitec/json.h"

#define GRANITE_ANSWER_FORMAT "granite-answer/1"

/* What a package says it makes: one kind each, and where. */
typedef struct granite_make {
    const char *kind;
    const char *what;
    /* Where it goes: a NULL-terminated list of folders or files. */
    const char *const *into;
    /* What it never makes, said so a host does not ask; or NULL. */
    const char *never;
} granite_make;

/* An answer with nothing said yet. */
json_object *granite_answer_new(void);
/* The first file written; it is also added to `paths`. */
void granite_answer_path(json_object *answer, const char *path);
/* A file written or removed. */
void granite_answer_add_path(json_object *answer, const char *path);
void granite_answer_note(json_object *answer, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Something made: `kind` one of the package's makes, at `path` (NULL when it has none), `state`
 * one of written (NULL), rewritten, unchanged, removed. */
void granite_answer_made(json_object *answer, const char *kind, const char *name, const char *path,
                         const char *state);
void granite_answer_expected(json_object *answer, const char *kind, const char *name);
void granite_answer_problem(json_object *answer, const char *kind, const char *text);
void granite_answer_payload(json_object *answer, const char *path, const char *schema);

/* Every way `answer` breaks granite-answer/1, a sentence each, as Swift's AgentAnswerCheck says
 * them; `*count` of them (the caller frees each and the array). `makes` ends with a NULL kind;
 * `directory` is what relative paths are read against (NULL: the process's own folder). */
char **granite_answer_problems(const json_object *answer, const char *word, const granite_make *makes,
                               const char *directory, size_t *count);
/* A copy of `answer` with the standard's id and the agent's word written in. */
json_object *granite_answer_stamped(const json_object *answer, const char *word);
/* A path as a file: under `directory` when relative, `.` and `..` collapsed, and the symlinks of
 * its longest existing ancestor resolved (/tmp is /private/tmp on macOS). The caller frees it. */
char *granite_answer_resolved(const char *path, const char *directory);

#endif
