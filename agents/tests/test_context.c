/* Errors and refusals as they read, the cancel flag, the host, and a run's context. */
#include <stdatomic.h>

#include "granite_test.h"
#include "granitec/context.h"
#include "granitec/lock.h"

GRANITE_TEST(a_refusal_reads_as_its_one_reason_or_as_a_count_and_a_list) {
    granite_error *one = granite_refuse(GRANITE_REFUSAL_CALL, "say what to %s", "plate");
    GRANITE_ASSERT_EQ(one->type, GRANITE_ERROR_REFUSED);
    GRANITE_ASSERT_STR(one->said, "say what to plate");
    GRANITE_ASSERT_EQ(one->reason_count, 1);
    const char *reasons[] = {"no title", "no body"};
    granite_error *two = granite_refuse_reasons(GRANITE_REFUSAL_ANSWER, reasons, 2);
    GRANITE_ASSERT_STR(two->said, "2 problems, so nothing was done:\n- no title\n- no body");
    granite_error *own = granite_refuse_said(GRANITE_REFUSAL_BUILD, "the comic did not build", reasons, 2);
    GRANITE_ASSERT_STR(own->said, "the comic did not build");
    GRANITE_ASSERT_STR(own->reasons[1], "no body");
    granite_error *copy = granite_error_copy(own);
    GRANITE_ASSERT_STR(copy->said, own->said);
    GRANITE_ASSERT_EQ(copy->kind, GRANITE_REFUSAL_BUILD);
    granite_error *fault = granite_fault("the disk is full");
    GRANITE_ASSERT_EQ(fault->type, GRANITE_ERROR_FAULT);
    granite_error *cancelled = granite_cancelled();
    GRANITE_ASSERT_STR(cancelled->said, "cancelled");
    granite_error_free(one);
    granite_error_free(two);
    granite_error_free(own);
    granite_error_free(copy);
    granite_error_free(fault);
    granite_error_free(cancelled);
}

GRANITE_TEST(a_kind_reads_back_and_one_not_known_is_the_hosts) {
    for (int kind = GRANITE_REFUSAL_INPUT; kind <= GRANITE_REFUSAL_HOST; kind++)
        GRANITE_ASSERT_EQ(granite_refusal_kind_named(granite_refusal_kind_name((granite_refusal_kind)kind)), kind);
    GRANITE_ASSERT_STR(granite_refusal_kind_name(GRANITE_REFUSAL_REACH), "reach");
    GRANITE_ASSERT_EQ(granite_refusal_kind_named("weather"), GRANITE_REFUSAL_HOST);
}

GRANITE_TEST(the_hosts_model_says_what_went_wrong_as_rust_does) {
    granite_error *none = granite_host_no_model("plate");
    GRANITE_ASSERT_STR(none->said, "plate asks the host's model, and this host has none loaded");
    GRANITE_ASSERT_EQ(none->kind, GRANITE_REFUSAL_MODEL);
    granite_error *gone = granite_host_no_answer("the host is gone");
    GRANITE_ASSERT_STR(gone->said, "the host's model gave no answer: the host is gone");
    granite_error *bad = granite_host_bad_answer("not an object");
    GRANITE_ASSERT_STR(bad->said, "the host's model answered something that does not decode: not an object");
    granite_error_free(none);
    granite_error_free(gone);
    granite_error_free(bad);
}

static void count_wake(void *receiver, const void *value, void *arg) {
    atomic_fetch_add((atomic_int *)arg, 1);
}

GRANITE_TEST(a_cancel_is_set_once_and_wakes_who_listens) {
    long before = granite_objects_alive();
    granite_cancel *cancel = granite_cancel_new();
    atomic_int woken = 0, late = 0;
    void *listener = granite_anchor_new();
    granite_cancel_on(cancel, listener, count_wake, &woken, NULL);
    GRANITE_ASSERT(!granite_cancel_is_set(cancel));
    granite_cancel_set(cancel);
    granite_cancel_set(cancel);
    GRANITE_ASSERT(granite_cancel_is_set(cancel));
    GRANITE_ASSERT_EQ(atomic_load(&woken), 1);
    void *after = granite_anchor_new();
    granite_cancel_on(cancel, after, count_wake, &late, NULL);
    GRANITE_ASSERT_EQ(atomic_load(&late), 1);
    granite_release(listener);
    granite_release(after);
    granite_release(cancel);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

/* A model that answers from what it was told, so a test can see what reached it. */
typedef struct echo_model {
    granite_model base;
    int asks;
} echo_model;

static granite_error *echo_object(granite_model *model, const json_object *schema, const char *system,
                                  const char *user, uint32_t max_tokens, double seconds, granite_cancel *cancel,
                                  json_object **object) {
    ((echo_model *)model)->asks++;
    *object = json_object_new_object();
    granite_json_set_string(*object, "system", system);
    granite_json_set_string(*object, "user", user);
    granite_json_set(*object, "maxTokens", json_object_new_int64(max_tokens));
    granite_json_set(*object, "schema", granite_json_copy(schema));
    return NULL;
}

static granite_error *echo_text(granite_model *model, const char *system, const char *user, uint32_t max_tokens,
                                double seconds, granite_cancel *cancel, char **text) {
    ((echo_model *)model)->asks++;
    *text = granite_strdupf("%s / %s", system, user);
    return NULL;
}

static const granite_model_methods echo_methods = {echo_object, echo_text};
static const granite_class echo_class = {"echo model", sizeof(echo_model), NULL, NULL};

typedef struct sunk {
    json_object *events;
    granite_buf notes;
} sunk;

static void sink(void *arg, json_object *event) {
    json_object_array_add(((sunk *)arg)->events, event);
}

static void note(void *arg, const char *line) {
    granite_buf_appendf(&((sunk *)arg)->notes, "%s;", line);
}

GRANITE_TEST(a_context_delivers_notes_and_asks_its_hosts_model) {
    long before = granite_objects_alive();
    sunk seen = {json_object_new_array(), {0}};
    echo_model *model = granite_object_new(&echo_class);
    model->base.methods = &echo_methods;
    granite_host *host = granite_host_new("/work", "draw a cat", &model->base, note, &seen, NULL);
    int compute = 7;
    granite_context *context = granite_context_new("plate-0000abcd", "plate", host, NULL, &compute, sink, &seen, NULL);
    GRANITE_ASSERT_STR(granite_context_run(context), "plate-0000abcd");
    GRANITE_ASSERT_STR(granite_context_directory(context), "/work");
    GRANITE_ASSERT_STR(granite_context_ask(context), "draw a cat");
    GRANITE_ASSERT(granite_context_compute(context) == &compute);
    granite_deliver(context, json_object_new_int(1));
    granite_deliver(context, json_object_new_int(2));
    granite_note(context, "round %d", 1);
    GRANITE_ASSERT_EQ(json_object_array_length(seen.events), 2);
    GRANITE_ASSERT_STR(granite_buf_str(&seen.notes), "round 1;");

    json_object *schema = json_object_new_object();
    granite_json_set_string(schema, "type", "object");
    json_object *answer = NULL;
    GRANITE_ASSERT(granite_ask_object(context, schema, "be brief", "a cat", 300, 30, &answer) == NULL);
    char *written = granite_json_write(answer, NULL);
    GRANITE_ASSERT_STR(written, "{\"maxTokens\":300,\"schema\":{\"type\":\"object\"},\"system\":\"be brief\",\"user\":\"a cat\"}");
    free(written);
    static const granite_field wanted_fields[] = {{"maxTokens", {GRANITE_SCHEMA_INTEGER, NULL, NULL, NULL}, true, NULL},
                                                  {NULL, {0}, false, NULL}};
    static const granite_schema wanted = {GRANITE_SCHEMA_OBJECT, NULL, NULL, wanted_fields};
    json_object *typed = NULL;
    GRANITE_ASSERT(granite_ask_schema(context, &wanted, "s", "u", 9, 1, &typed) == NULL);
    granite_json_put(typed);
    static const granite_field strict_fields[] = {{"missing", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, true, NULL},
                                                  {NULL, {0}, false, NULL}};
    static const granite_schema strict = {GRANITE_SCHEMA_OBJECT, NULL, NULL, strict_fields};
    granite_error *undecoded = granite_ask_schema(context, &strict, "s", "u", 9, 1, &typed);
    GRANITE_ASSERT(undecoded && typed == NULL);
    GRANITE_ASSERT_STR(undecoded ? undecoded->said : NULL,
                       "the host's model answered something that does not decode: missing field `missing`");
    granite_error_free(undecoded);
    char *text = NULL;
    GRANITE_ASSERT(granite_ask_text(context, "sys", "usr", 10, 5, &text) == NULL);
    GRANITE_ASSERT_STR(text, "sys / usr");
    free(text);

    granite_cancel_set(granite_context_cancel(context));
    GRANITE_ASSERT(granite_context_is_cancelled(context));
    granite_error *refused = granite_ask_text(context, "sys", "usr", 10, 5, &text);
    GRANITE_ASSERT(refused && refused->kind == GRANITE_REFUSAL_MODEL);
    GRANITE_ASSERT_EQ(model->asks, 4);
    granite_error_free(refused);

    granite_json_put(answer);
    granite_json_put(schema);
    granite_release(context);
    granite_release(host);
    granite_release(model);
    granite_json_put(seen.events);
    granite_buf_free(&seen.notes);
    GRANITE_ASSERT_EQ(granite_objects_alive(), before);
}

GRANITE_TEST(asking_a_host_without_a_model_is_a_model_refusal) {
    granite_context *context = granite_context_new("r", "plate", NULL, NULL, NULL, NULL, NULL, NULL);
    char *text = NULL;
    granite_error *refused = granite_ask_text(context, "s", "u", 1, 1, &text);
    GRANITE_ASSERT(refused != NULL);
    GRANITE_ASSERT_STR(refused->said, "plate asks the host's model, and this host has none loaded");
    GRANITE_ASSERT(text == NULL);
    granite_deliver(context, json_object_new_int(3)); /* no sink: let go, not leaked */
    granite_error_free(refused);
    granite_release(context);
}

int main(void) {
    GRANITE_RUN(a_refusal_reads_as_its_one_reason_or_as_a_count_and_a_list);
    GRANITE_RUN(a_kind_reads_back_and_one_not_known_is_the_hosts);
    GRANITE_RUN(the_hosts_model_says_what_went_wrong_as_rust_does);
    GRANITE_RUN(a_cancel_is_set_once_and_wakes_who_listens);
    GRANITE_RUN(a_context_delivers_notes_and_asks_its_hosts_model);
    GRANITE_RUN(asking_a_host_without_a_model_is_a_model_refusal);
    GRANITE_TEST_MAIN_END();
}
