/* `parrot`: a package agent that exercises every message of the wire. It says its text back, notes
 * as it goes, asks the host's model when told, waits when told, refuses, faults, and crashes when
 * told. GraniteRust's granite_conformance::Parrot, line for line: the same entry, the same answers,
 * the same sentences. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <granitec/granitec.h>
#include <granitec/server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const granite_field parrot_fields[] = {
    {"text", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, true, "what to say"},
    {"notes", {GRANITE_SCHEMA_INTEGER, NULL, NULL, NULL}, false, NULL},
    {"askModel", {GRANITE_SCHEMA_BOOLEAN, NULL, NULL, NULL}, false, NULL},
    {"delay", {GRANITE_SCHEMA_NUMBER, NULL, NULL, NULL}, false, NULL},
    {"crash", {GRANITE_SCHEMA_BOOLEAN, NULL, NULL, NULL}, false, NULL},
    {"fail", {GRANITE_SCHEMA_BOOLEAN, NULL, NULL, NULL}, false, NULL},
    {"fault", {GRANITE_SCHEMA_BOOLEAN, NULL, NULL, NULL}, false, NULL},
    {"directory", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, false, NULL},
    {NULL, {0}, false, NULL},
};
static const granite_schema parrot_input = {GRANITE_SCHEMA_OBJECT, NULL, NULL, parrot_fields};

static bool told(const json_object *input, const char *key) {
    json_object *value = granite_json_get(input, key);
    return value && json_object_get_boolean(value);
}

/* A double as Rust's {} writes it: the shortest digits, never an exponent, no ".0". */
static void rust_display(double value, char *out, size_t n) {
    char text[64];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(text, sizeof text, "%.*e", precision - 1, value);
        if (strtod(text, NULL) == value) break;
    }
    char digits[24];
    size_t count = 0, k = 0;
    const char *c = text;
    if (*c == '-') out[k++] = *c++;
    for (; *c && *c != 'e'; c++)
        if (*c >= '0' && *c <= '9') digits[count++] = *c;
    int exponent = atoi(c + 1);
    while (count > 1 && digits[count - 1] == '0') count--;
    if (value == 0) count = 1, exponent = 0;
    if (exponent < 0) {
        k += (size_t)snprintf(out + k, n - k, "0.");
        for (int i = 1; i < -exponent && k + 1 < n; i++) out[k++] = '0';
        for (size_t i = 0; i < count && k + 1 < n; i++) out[k++] = digits[i];
    } else {
        for (size_t i = 0; i < count && k + 1 < n; i++) {
            if ((int)i == exponent + 1) out[k++] = '.';
            out[k++] = digits[i];
        }
        for (int i = (int)count; i <= exponent && k + 1 < n; i++) out[k++] = '0';
    }
    out[k] = '\0';
}

static granite_error *parrot_run(granite_context *context, const json_object *input) {
    printf("this line must not reach the wire\n");
    fflush(stdout);
    json_object *notes = granite_json_get(input, "notes");
    for (int64_t i = 0; notes && i < json_object_get_int64(notes); i++) granite_note(context, "note %lld", (long long)i + 1);
    if (told(input, "crash")) abort();
    if (told(input, "fail")) {
        const char *reasons[] = {"told to fail: leave out `fail`"};
        return granite_refuse_said(GRANITE_REFUSAL_CALL, "told to fail", reasons, 1);
    }
    if (told(input, "fault")) return granite_fault("it broke");
    json_object *delay = granite_json_get(input, "delay");
    if (delay) {
        char said[64];
        rust_display(json_object_get_double(delay), said, sizeof said);
        granite_note(context, "waiting %s s as pid %d", said, (int)getpid());
        double until = granite_now() + json_object_get_double(delay);
        while (granite_now() < until && !granite_context_is_cancelled(context)) granite_sleep(0.01);
    }
    json_object *answer = granite_answer_new();
    granite_json_set_string(answer, "said", granite_json_get_string(input, "text"));
    granite_json_set(answer, "modelSaid", NULL);
    const char *directory = granite_json_get_string(input, "directory");
    granite_json_set(answer, "directory", directory ? json_object_new_string(directory) : NULL);
    const char *host_directory = granite_context_directory(context);
    granite_json_set(answer, "hostDirectory", host_directory ? json_object_new_string(host_directory) : NULL);
    const char *ask = granite_context_ask(context);
    granite_json_set(answer, "ask", ask ? json_object_new_string(ask) : NULL);
    granite_json_set(answer, "pid", json_object_new_int64(getpid()));
    if (told(input, "askModel")) {
        static const char schema_text[] =
            "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}},\"required\":[\"n\"]}";
        json_object *schema = NULL, *object = NULL;
        granite_json_read(schema_text, sizeof schema_text - 1, &schema, NULL);
        granite_error *error = granite_ask_object(context, schema, "Count.", "How many?", 16, 5.0, &object);
        granite_json_put(schema);
        if (error) {
            granite_json_put(answer);
            return error;
        }
        json_object *n = granite_json_get(object, "n");
        if (granite_json_is_integer(n)) granite_json_set(answer, "modelSaid", json_object_new_int64(json_object_get_int64(n)));
        granite_json_put(object);
    }
    granite_deliver(context, answer);
    return NULL;
}

const granite_package parrot = {
    .word = "parrot",
    .summary = "says its text back, in a process of its own.",
    .module = "granite_conformance",
    .type = "granite_conformance.Parrot",
    .guide = "Give it text; it says it back.",
    .input = &parrot_input,
    .run = parrot_run,
};

static const granite_package *const roster[] = {&parrot, NULL};

int main(int argc, char **argv) {
    return granite_server_main(argc, argv, roster);
}
