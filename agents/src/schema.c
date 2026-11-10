/* What an input takes. See granitec/schema.h. */
#include "granitec/schema.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "granitec/buf.h"

static const char *type_word(granite_schema_type type) {
    switch (type) {
    case GRANITE_SCHEMA_STRING: return "string";
    case GRANITE_SCHEMA_INTEGER: return "integer";
    case GRANITE_SCHEMA_NUMBER: return "number";
    case GRANITE_SCHEMA_BOOLEAN: return "boolean";
    default: return "any";
    }
}

static json_object *choices_array(const char *const *choices) {
    json_object *values = json_object_new_array();
    for (const char *const *c = choices; c && *c; c++) json_object_array_add(values, json_object_new_string(*c));
    return values;
}

json_object *granite_schema_value(const granite_schema *schema) {
    static const granite_schema any = {GRANITE_SCHEMA_ANY, NULL, NULL, NULL};
    if (!schema) schema = &any;
    json_object *shaped = NULL;
    switch (schema->type) {
    case GRANITE_SCHEMA_CHOICE:
        shaped = json_object_new_object();
        granite_json_set(shaped, "choice", choices_array(schema->choices));
        return shaped;
    case GRANITE_SCHEMA_ARRAY:
        shaped = json_object_new_object();
        granite_json_set(shaped, "array", granite_schema_value(schema->items));
        return shaped;
    case GRANITE_SCHEMA_OBJECT: {
        json_object *fields = json_object_new_array();
        for (const granite_field *f = schema->fields; f && f->name; f++) {
            json_object *field = json_object_new_object();
            granite_json_set_string(field, "name", f->name);
            granite_json_set(field, "required", json_object_new_boolean(f->required));
            granite_json_set(field, "schema", granite_schema_value(&f->schema));
            json_object_array_add(fields, field);
        }
        shaped = json_object_new_object();
        granite_json_set(shaped, "object", fields);
        return shaped;
    }
    default: return json_object_new_string(type_word(schema->type));
    }
}

json_object *granite_schema_json_schema(const granite_schema *schema, bool hide_directory) {
    json_object *out = json_object_new_object();
    if (!schema) return out;
    switch (schema->type) {
    case GRANITE_SCHEMA_ANY: return out;
    case GRANITE_SCHEMA_CHOICE:
        granite_json_set_string(out, "type", "string");
        granite_json_set(out, "enum", choices_array(schema->choices));
        return out;
    case GRANITE_SCHEMA_ARRAY:
        granite_json_set_string(out, "type", "array");
        granite_json_set(out, "items", granite_schema_json_schema(schema->items, false));
        return out;
    case GRANITE_SCHEMA_OBJECT: {
        json_object *properties = json_object_new_object(), *required = json_object_new_array();
        for (const granite_field *f = schema->fields; f && f->name; f++) {
            if (hide_directory && strcmp(f->name, GRANITE_DIRECTORY_FIELD) == 0) continue;
            json_object *property = granite_schema_json_schema(&f->schema, false);
            if (f->note) granite_json_set_string(property, "description", f->note);
            granite_json_set(properties, f->name, property);
            if (f->required) json_object_array_add(required, json_object_new_string(f->name));
        }
        granite_json_set_string(out, "type", "object");
        granite_json_set(out, "properties", properties);
        granite_json_set(out, "required", required);
        return out;
    }
    default: granite_json_set_string(out, "type", type_word(schema->type)); return out;
    }
}

json_object *granite_schema_notes(const granite_schema *schema) {
    json_object *notes = json_object_new_object();
    if (schema && schema->type == GRANITE_SCHEMA_OBJECT)
        for (const granite_field *f = schema->fields; f && f->name; f++)
            if (f->note) granite_json_set_string(notes, f->name, f->note);
    return notes;
}

bool granite_schema_takes_directory(const granite_schema *schema) {
    if (!schema || schema->type != GRANITE_SCHEMA_OBJECT) return false;
    for (const granite_field *f = schema->fields; f && f->name; f++)
        if (strcmp(f->name, GRANITE_DIRECTORY_FIELD) == 0) return true;
    return false;
}

/* A double as Rust's Display writes it, which never uses an exponent, with serde's decimal point. */
static void append_float(granite_buf *buf, double value) {
    if (isnan(value)) {
        granite_buf_append_str(buf, "NaN");
        return;
    }
    if (isinf(value)) {
        granite_buf_append_str(buf, value < 0 ? "-inf" : "inf");
        return;
    }
    char text[400];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(text, sizeof text, "%.*e", precision - 1, value);
        if (strtod(text, NULL) == value) break;
    }
    char digits[24];
    size_t n = 0;
    const char *c = text;
    if (*c == '-') {
        granite_buf_append_char(buf, '-');
        c++;
    }
    for (; *c && *c != 'e'; c++)
        if (*c >= '0' && *c <= '9') digits[n++] = *c;
    int exponent = *c == 'e' ? atoi(c + 1) : 0;
    while (n > 1 && digits[n - 1] == '0') n--;
    if (exponent < 0) {
        granite_buf_append_str(buf, "0.");
        for (int i = 1; i < -exponent; i++) granite_buf_append_char(buf, '0');
        granite_buf_append(buf, digits, n);
    } else if ((size_t)exponent + 1 >= n) {
        granite_buf_append(buf, digits, n);
        for (size_t i = n; i <= (size_t)exponent; i++) granite_buf_append_char(buf, '0');
        granite_buf_append_str(buf, ".0");
    } else {
        granite_buf_append(buf, digits, (size_t)exponent + 1);
        granite_buf_append_char(buf, '.');
        granite_buf_append(buf, digits + exponent + 1, n - (size_t)exponent - 1);
    }
}

/* A string as Rust's {:?} quotes it. */
static void append_debug_string(granite_buf *buf, const char *text, size_t n) {
    granite_buf_append_char(buf, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '"': granite_buf_append_str(buf, "\\\""); break;
        case '\\': granite_buf_append_str(buf, "\\\\"); break;
        case '\n': granite_buf_append_str(buf, "\\n"); break;
        case '\r': granite_buf_append_str(buf, "\\r"); break;
        case '\t': granite_buf_append_str(buf, "\\t"); break;
        case '\0': granite_buf_append_str(buf, "\\0"); break;
        default:
            if (c < 0x20 || c == 0x7f) granite_buf_appendf(buf, "\\u{%x}", c);
            else granite_buf_append_char(buf, (char)c);
        }
    }
    granite_buf_append_char(buf, '"');
}

char *granite_json_unexpected(const json_object *value) {
    granite_buf said = {0};
    json_object *v = (json_object *)value;
    switch (json_object_get_type(v)) {
    case json_type_null: granite_buf_append_str(&said, "null"); break;
    case json_type_boolean: granite_buf_appendf(&said, "boolean `%s`", json_object_get_boolean(v) ? "true" : "false"); break;
    case json_type_int: {
        int64_t i = json_object_get_int64(v);
        if (i == INT64_MAX) granite_buf_appendf(&said, "integer `%" PRIu64 "`", json_object_get_uint64(v));
        else granite_buf_appendf(&said, "integer `%" PRId64 "`", i);
        break;
    }
    case json_type_double:
        granite_buf_append_str(&said, "floating point `");
        append_float(&said, json_object_get_double(v));
        granite_buf_append_char(&said, '`');
        break;
    case json_type_string:
        granite_buf_append_str(&said, "string ");
        append_debug_string(&said, json_object_get_string(v), (size_t)json_object_get_string_len(v));
        break;
    case json_type_array: granite_buf_append_str(&said, "sequence"); break;
    case json_type_object: granite_buf_append_str(&said, "map"); break;
    }
    return granite_buf_take(&said);
}

static char *invalid_type(const json_object *value, const char *expected) {
    char *unexpected = granite_json_unexpected(value);
    char *said = granite_strdupf("invalid type: %s, expected %s", unexpected, expected);
    free(unexpected);
    return said;
}

/* serde's "expected one of `a`, `b`" for an unknown variant. */
static char *unknown_variant(const char *got, const char *const *choices) {
    granite_buf said = {0};
    granite_buf_appendf(&said, "unknown variant `%s`, ", got);
    size_t count = 0;
    for (const char *const *c = choices; c && *c; c++) count++;
    if (count == 0) {
        granite_buf_append_str(&said, "there are no variants");
    } else if (count == 1) {
        granite_buf_appendf(&said, "expected `%s`", choices[0]);
    } else if (count == 2) {
        granite_buf_appendf(&said, "expected `%s` or `%s`", choices[0], choices[1]);
    } else {
        granite_buf_append_str(&said, "expected one of ");
        for (size_t i = 0; i < count; i++) granite_buf_appendf(&said, "%s`%s`", i ? ", " : "", choices[i]);
    }
    return granite_buf_take(&said);
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char *granite_schema_check(const granite_schema *schema, const json_object *value) {
    if (!schema) return NULL;
    json_type type = json_object_get_type((json_object *)value);
    switch (schema->type) {
    case GRANITE_SCHEMA_ANY: return NULL;
    case GRANITE_SCHEMA_STRING: return type == json_type_string ? NULL : invalid_type(value, "a string");
    case GRANITE_SCHEMA_BOOLEAN: return type == json_type_boolean ? NULL : invalid_type(value, "a boolean");
    case GRANITE_SCHEMA_INTEGER: return type == json_type_int ? NULL : invalid_type(value, "an integer");
    case GRANITE_SCHEMA_NUMBER: return granite_json_is_number(value) ? NULL : invalid_type(value, "a number");
    case GRANITE_SCHEMA_CHOICE: {
        if (type != json_type_string) return invalid_type(value, "string or map");
        const char *got = json_object_get_string((json_object *)value);
        for (const char *const *c = schema->choices; c && *c; c++)
            if (strcmp(*c, got) == 0) return NULL;
        return unknown_variant(got, schema->choices);
    }
    case GRANITE_SCHEMA_ARRAY: {
        if (type != json_type_array) return invalid_type(value, "a sequence");
        size_t n = json_object_array_length((json_object *)value);
        for (size_t i = 0; i < n; i++) {
            char *said = granite_schema_check(schema->items, json_object_array_get_idx((json_object *)value, i));
            if (said) return said;
        }
        return NULL;
    }
    case GRANITE_SCHEMA_OBJECT: {
        if (type != json_type_object) return invalid_type(value, "an object");
        /* serde visits a map's keys in order (serde_json's Map is sorted), then names the first
         * required field it did not see, in the order the type declares them. */
        size_t n = (size_t)json_object_object_length((json_object *)value), i = 0;
        const char **keys = granite_malloc((n ? n : 1) * sizeof *keys);
        json_object_object_foreach((json_object *)value, key, unused) {
            (void)unused;
            keys[i++] = key;
        }
        qsort(keys, i, sizeof *keys, compare_names);
        char *said = NULL;
        for (size_t k = 0; k < i && !said; k++) {
            for (const granite_field *f = schema->fields; f && f->name; f++) {
                if (strcmp(f->name, keys[k]) != 0) continue;
                json_object *member = granite_json_get(value, f->name);
                if (!f->required && member == NULL) break; /* an Option reads null as none */
                said = granite_schema_check(&f->schema, member);
                break;
            }
        }
        free(keys);
        if (said) return said;
        for (const granite_field *f = schema->fields; f && f->name; f++)
            if (f->required && !granite_json_has(value, f->name)) return granite_strdupf("missing field `%s`", f->name);
        return NULL;
    }
    }
    return NULL;
}
