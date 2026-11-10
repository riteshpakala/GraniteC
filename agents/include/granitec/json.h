/* JSON as the standard reads and writes it, over json-c trees.
 *
 * Reading is GraniteC's own, strict, and reads as serde_json does (GraniteRust's reader): the
 * same values, the same refusals, and the same sentences with the same line and column, so a C
 * host and a Rust host say the same thing about the same bad line. Writing is GraniteC's own too:
 * every object's keys sorted at every depth, slashes never escaped, serde's escapes, and numbers
 * laid out as serde_json lays them out (shortest digits that read back; fixed notation for
 * decimal exponents -5 to 15, else 1.5e+16 and 1e-7).
 *
 * PIN: A TREE HAS ONE OWNER THREAD AT A TIME. json-c's counts are not atomic: hand a tree across
 * threads, never share it.
 * PIN: ONLY granite_json_put FREES A TREE. It refuses under a granite_lock, like a release
 * (`make lint` holds that nothing else calls json_object_put).
 * PIN: JSON null IS NULL. json-c spells null as a NULL pointer, so reading says whether it read
 * through its return value, never through the value it hands back. */
#ifndef GRANITEC_JSON_H
#define GRANITEC_JSON_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "granitec/buf.h"

/* Reads one JSON text. True with the value in `*value` (NULL for null); false with a sentence in
 * `*error` (the caller frees it), worded as serde_json words it. */
bool granite_json_read(const char *bytes, size_t len, json_object **value, char **error);

/* The value as sorted, compact JSON (serde_json::to_vec of a sorted value). The caller frees it;
 * `len` (optional) says how long. */
char *granite_json_write(const json_object *value, size_t *len);
/* As granite_json_write, indented two spaces (serde_json::to_vec_pretty): a record on disk. */
char *granite_json_write_pretty(const json_object *value, size_t *len);
/* Appends the sorted, compact JSON of `value`. */
void granite_json_append(granite_buf *buf, const json_object *value);
/* Appends `text` (`n` bytes) as a JSON string, quotes included. */
void granite_json_append_string(granite_buf *buf, const char *text, size_t n);
/* Appends `value` as serde_json writes an f64 (null when it is not finite). */
void granite_json_append_double(granite_buf *buf, double value);

/* Frees `value` (NULL is fine). Aborts under a granite_lock. */
void granite_json_put(json_object *value);
/* A number as Swift writes a Double: whole numbers below 2^53 as integers (300, not 300.0). */
json_object *granite_json_number(double value);
/* A deep copy (NULL for null). */
json_object *granite_json_copy(const json_object *value);
/* Whether two values are the same JSON: numbers by value (1 and 1.0 alike), objects by keys. */
bool granite_json_equal(const json_object *a, const json_object *b);

/* `key` of an object: the value, or NULL when there is none or it is null. */
json_object *granite_json_get(const json_object *object, const char *key);
/* Whether an object has `key` at all, null or not. */
bool granite_json_has(const json_object *object, const char *key);
/* `key` of an object when it is a string, else NULL. */
const char *granite_json_get_string(const json_object *object, const char *key);
/* Sets `key` to `value` (taken; NULL sets null). */
void granite_json_set(json_object *object, const char *key, json_object *value);
/* Sets `key` to a string copy of `text`; NULL text sets nothing. */
void granite_json_set_string(json_object *object, const char *key, const char *text);
/* Whether `value` is an integer (json-c int, signed or unsigned). */
bool granite_json_is_integer(const json_object *value);
/* Whether `value` is any number. */
bool granite_json_is_number(const json_object *value);

#endif
