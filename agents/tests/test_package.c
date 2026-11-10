/* A package call in this process: the input read and checked, the run hosted, the answer checked,
 * stamped and written; what a host reads when it was not answered; and the entry --describe says. */
#include <pthread.h>
#include <stdatomic.h>

#include "granite_test.h"
#include "granitec/granitec.h"

static const granite_field echo_fields[] = {
    {"text", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, true, "what to say back"},
    {"mode", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, false, NULL},
    {NULL, {0}, false, NULL},
};
static const granite_schema echo_input = {GRANITE_SCHEMA_OBJECT, NULL, NULL, echo_fields};
static atomic_int computes = 0, fail_next_compute = 0;
static int the_compute = 42;

static granite_error *echo_compute(void **compute) {
    atomic_fetch_add(&computes, 1);
    if (atomic_exchange(&fail_next_compute, 0)) return granite_fault("no GPU here");
    *compute = &the_compute;
    return NULL;
}

static granite_error *echo_run(granite_context *context, const json_object *input) {
    const char *mode = granite_json_get_string(input, "mode");
    mode = mode ? mode : "good";
    if (granite_context_compute(context) != &the_compute) return granite_fault("the compute did not reach the run");
    if (strcmp(mode, "refuse") == 0) return granite_refuse(GRANITE_REFUSAL_CALL, "`text` is empty: say something");
    if (strcmp(mode, "fault") == 0) return granite_fault("the disk is full");
    if (strcmp(mode, "none") == 0) return NULL;
    if (strcmp(mode, "array") == 0) {
        granite_deliver(context, json_object_new_array());
        return NULL;
    }
    if (strcmp(mode, "slow") == 0) {
        while (!granite_context_is_cancelled(context)) granite_sleep(0.002);
        return NULL;
    }
    json_object *answer = strcmp(mode, "bad") == 0 ? json_object_new_object() : granite_answer_new();
    granite_json_set_string(answer, "said", granite_json_get_string(input, "text"));
    granite_deliver(context, json_object_new_object()); /* an early event the answer is not read off */
    granite_deliver(context, answer);
    return NULL;
}

static const char *const card_into[] = {"cards/", NULL};
static const granite_make echo_makes[] = {{"card", "a card", card_into, NULL}, {NULL, NULL, NULL, NULL}};

static const granite_package echo = {
    .word = "echo",
    .summary = "Says it back.",
    .module = "Echo",
    .guide = "Give it text.",
    .makes = echo_makes,
    .input = &echo_input,
    .compute = echo_compute,
    .run = echo_run,
};

static granite_error *perform(const granite_package *package, const char *input, char **answer) {
    return granite_package_perform(package, input, strlen(input), NULL, NULL, answer, NULL);
}

GRANITE_TEST(a_conforming_call_answers_checked_stamped_and_sorted) {
    char *answer = NULL;
    granite_error *error = perform(&echo, "{\"text\":\"hi\"}", &answer);
    GRANITE_ASSERT(error == NULL);
    GRANITE_ASSERT_STR(answer, "{\"agent\":{\"word\":\"echo\"},\"expected\":[],\"format\":\"granite-answer/1\",\"made\":[],"
                               "\"path\":null,\"paths\":[],\"payloads\":[],\"problems\":[],\"said\":\"hi\"}");
    free(answer);
    granite_error_free(error);
}

static void expect_refusal(const char *input, granite_refusal_kind kind, const char *said, const char *reason) {
    char *answer = NULL;
    granite_error *error = perform(&echo, input, &answer);
    if (!error) {
        GRANITE_FAIL("%s: answered %s", input, answer);
        free(answer);
        return;
    }
    if (error->type != GRANITE_ERROR_REFUSED || error->kind != kind)
        GRANITE_FAIL("%s: ended %d (kind %d), said %s", input, error->type, error->kind, error->said);
    if (strcmp(error->said, said) != 0) GRANITE_FAIL("%s: said \"%s\", expected \"%s\"", input, error->said, said);
    if (reason && (error->reason_count < 1 || strcmp(error->reasons[0], reason) != 0))
        GRANITE_FAIL("%s: its first reason is \"%s\"", input, error->reason_count ? error->reasons[0] : "(none)");
    granite_error_free(error);
}

GRANITE_TEST(input_that_does_not_read_is_an_input_refusal_in_rusts_sentence) {
    expect_refusal("not json", GRANITE_REFUSAL_INPUT, "echo could not read its input: expected ident at line 1 column 2",
                   "expected ident at line 1 column 2");
    expect_refusal("{\"mode\":\"good\"}", GRANITE_REFUSAL_INPUT, "echo could not read its input: missing field `text`",
                   "missing field `text`");
    expect_refusal("{\"text\":5}", GRANITE_REFUSAL_INPUT,
                   "echo could not read its input: invalid type: integer `5`, expected a string", NULL);
}

GRANITE_TEST(what_the_run_returns_is_what_the_host_reads) {
    expect_refusal("{\"text\":\"\",\"mode\":\"refuse\"}", GRANITE_REFUSAL_CALL, "`text` is empty: say something",
                   "`text` is empty: say something");
    char *answer = NULL;
    granite_error *fault = perform(&echo, "{\"text\":\"x\",\"mode\":\"fault\"}", &answer);
    GRANITE_ASSERT(fault && fault->type == GRANITE_ERROR_FAULT);
    GRANITE_ASSERT_STR(fault ? fault->said : NULL, "the disk is full");
    granite_error_free(fault);
}

GRANITE_TEST(an_answer_that_is_missing_or_breaks_the_standard_is_an_answer_refusal) {
    expect_refusal("{\"text\":\"x\",\"mode\":\"none\"}", GRANITE_REFUSAL_ANSWER, "echo finished without an answer", NULL);
    expect_refusal("{\"text\":\"x\",\"mode\":\"array\"}", GRANITE_REFUSAL_ANSWER,
                   "echo answered something that is not an object", NULL);
    expect_refusal("{\"text\":\"x\",\"mode\":\"bad\"}", GRANITE_REFUSAL_ANSWER,
                   "echo's answer breaks granite-answer/1: `paths` is missing: an answer names every file it wrote, `[]` when none",
                   "`paths` is missing: an answer names every file it wrote, `[]` when none");
}

static granite_error *flaky_compute(void **compute) {
    return echo_compute(compute);
}

static const granite_package flaky = {
    .word = "flaky", .summary = "Fails to start once.", .module = "Flaky", .input = &echo_input,
    .compute = flaky_compute, .run = echo_run,
};

GRANITE_TEST(compute_is_made_once_and_tried_again_after_it_fails) {
    atomic_store(&fail_next_compute, 1);
    int before = atomic_load(&computes);
    char *answer = NULL;
    granite_error *failed = perform(&flaky, "{\"text\":\"a\"}", &answer);
    GRANITE_ASSERT(failed && failed->type == GRANITE_ERROR_FAULT);
    GRANITE_ASSERT_STR(failed ? failed->said : NULL, "PackageRun-1 failed: no GPU here");
    granite_error_free(failed);
    for (int i = 0; i < 3; i++) {
        granite_error *error = perform(&flaky, "{\"text\":\"a\"}", &answer);
        GRANITE_ASSERT(error == NULL);
        granite_error_free(error);
        free(answer);
    }
    GRANITE_ASSERT_EQ(atomic_load(&computes) - before, 2);
}

typedef struct slow_call {
    granite_cancel *cancel;
    granite_error *result;
} slow_call;

static void *perform_slowly(void *pointer) {
    slow_call *call = pointer;
    char *answer = NULL;
    const char *input = "{\"text\":\"x\",\"mode\":\"slow\"}";
    call->result = granite_package_perform(&echo, input, strlen(input), NULL, call->cancel, &answer, NULL);
    free(answer);
    return NULL;
}

GRANITE_TEST(a_call_is_cancelled_by_its_flag_before_or_while_it_runs) {
    slow_call call = {granite_cancel_new(), NULL};
    pthread_t thread;
    pthread_create(&thread, NULL, perform_slowly, &call);
    granite_sleep(0.05);
    granite_cancel_set(call.cancel);
    pthread_join(thread, NULL);
    GRANITE_ASSERT(call.result && call.result->type == GRANITE_ERROR_CANCELLED);
    granite_error_free(call.result);
    granite_release(call.cancel);

    slow_call early = {granite_cancel_new(), NULL};
    granite_cancel_set(early.cancel);
    perform_slowly(&early);
    GRANITE_ASSERT(early.result && early.result->type == GRANITE_ERROR_CANCELLED);
    granite_error_free(early.result);
    granite_release(early.cancel);
}

GRANITE_TEST(an_entry_says_what_rust_says_and_leaves_out_what_is_empty) {
    char *entry = granite_json_write(granite_package_entry(&echo), NULL);
    GRANITE_ASSERT_STR(entry, "{\"fields\":{\"text\":\"what to say back\"},\"guide\":\"Give it text.\",\"kind\":\"package\","
                              "\"lanes\":2,\"makes\":[{\"into\":[\"cards/\"],\"kind\":\"card\",\"what\":\"a card\"}],"
                              "\"module\":\"Echo\",\"schema\":{\"object\":[{\"name\":\"text\",\"required\":true,\"schema\":\"string\"},"
                              "{\"name\":\"mode\",\"required\":false,\"schema\":\"string\"}]},\"summary\":\"Says it back.\","
                              "\"type\":\"Echo.echo\",\"word\":\"echo\"}");
    free(entry);
    const granite_package bare = {.word = "bare", .summary = "s", .module = "M", .type = "M.Bare", .lanes = GRANITE_LANES_UNBOUNDED};
    json_object *described = granite_package_entry(&bare);
    char *text = granite_json_write(described, NULL);
    GRANITE_ASSERT_STR(text, "{\"fields\":{},\"kind\":\"package\",\"module\":\"M\",\"schema\":\"any\",\"summary\":\"s\","
                             "\"type\":\"M.Bare\",\"word\":\"bare\"}");
    free(text);
    granite_json_put(described);
}

GRANITE_TEST(a_roster_keeps_the_first_package_of_a_word) {
    const granite_package echo_again = {.word = "echo", .summary = "a second echo", .module = "Other"};
    const granite_package *const roster[] = {&echo, &flaky, &echo_again, NULL};
    json_object *entries = granite_roster_entries(roster);
    GRANITE_ASSERT_EQ(json_object_array_length(entries), 2);
    GRANITE_ASSERT(granite_roster_find(roster, "echo") == &echo);
    GRANITE_ASSERT(granite_roster_find(roster, "nobody") == NULL);
    granite_json_put(entries);
}

int main(void) {
    GRANITE_RUN(a_conforming_call_answers_checked_stamped_and_sorted);
    GRANITE_RUN(input_that_does_not_read_is_an_input_refusal_in_rusts_sentence);
    GRANITE_RUN(what_the_run_returns_is_what_the_host_reads);
    GRANITE_RUN(an_answer_that_is_missing_or_breaks_the_standard_is_an_answer_refusal);
    GRANITE_RUN(compute_is_made_once_and_tried_again_after_it_fails);
    GRANITE_RUN(a_call_is_cancelled_by_its_flag_before_or_while_it_runs);
    GRANITE_RUN(an_entry_says_what_rust_says_and_leaves_out_what_is_empty);
    GRANITE_RUN(a_roster_keeps_the_first_package_of_a_word);
    GRANITE_TEST_MAIN_END();
}
