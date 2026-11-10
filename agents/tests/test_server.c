/* One call a server answers, ported from GraniteRust's server tests: a hello first, one output or
 * failed last, the host's model asked across the channel, and each way a host can end a call with
 * the exit it ends with. The runner serves on a thread; the test is its host. */
#include <pthread.h>

#include "granite_test.h"
#include "granitec/granitec.h"
#include "granitec/server.h"

static const granite_field echo_fields[] = {
    {"text", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, true, NULL},
    {"delay", {GRANITE_SCHEMA_NUMBER, NULL, NULL, NULL}, false, NULL},
    {NULL, {0}, false, NULL},
};
static const granite_schema echo_input = {GRANITE_SCHEMA_OBJECT, NULL, NULL, echo_fields};

static granite_error *echo_run(granite_context *context, const json_object *input) {
    json_object *delay = granite_json_get(input, "delay");
    double until = granite_now() + (delay ? json_object_get_double(delay) : 0);
    while (granite_now() < until && !granite_context_is_cancelled(context)) granite_sleep(0.002);
    json_object *answer = granite_answer_new();
    granite_json_set_string(answer, "said", granite_json_get_string(input, "text"));
    granite_deliver(context, answer);
    return NULL;
}

static const granite_package echo = {.word = "echo", .summary = "Says it back.", .module = "Common",
                                     .input = &echo_input, .run = echo_run};

static const granite_field hosted_fields[] = {
    {"ask_model", {GRANITE_SCHEMA_BOOLEAN, NULL, NULL, NULL}, false, NULL},
    {NULL, {0}, false, NULL},
};
static const granite_schema hosted_input = {GRANITE_SCHEMA_OBJECT, NULL, NULL, hosted_fields};

static granite_error *hosted_run(granite_context *context, const json_object *input) {
    granite_note(context, "offered");
    json_object *answer = granite_answer_new();
    json_object *ask_model = granite_json_get(input, "ask_model");
    if (ask_model && json_object_get_boolean(ask_model)) {
        json_object *schema = json_object_new_object(), *object = NULL;
        granite_json_set_string(schema, "type", "object");
        granite_error *error = granite_ask_object(context, schema, "system", "user", 3000, 300, &object);
        granite_json_put(schema);
        if (error) {
            granite_json_put(answer);
            return error;
        }
        granite_json_set(answer, "n", granite_json_copy(granite_json_get(object, "n")));
        granite_json_put(object);
    }
    granite_json_set_string(answer, "directory", granite_context_directory(context));
    granite_deliver(context, answer);
    return NULL;
}

static const granite_package hosted = {.word = "hosted", .summary = "Asks its host.", .module = "Common",
                                       .input = &hosted_input, .run = hosted_run};

static const granite_package *const roster[] = {&echo, &hosted, NULL};

typedef struct serving {
    granite_channel *runner;
    granite_exit exit;
} serving;

static void *serve(void *pointer) {
    serving *s = pointer;
    s->exit = granite_server_serve(s->runner, roster, NULL);
    return NULL;
}

static granite_wire *perform(const char *word, const char *input, bool model) {
    granite_wire *message = granite_wire_new(GRANITE_WIRE_PERFORM);
    message->word = granite_strdup(word);
    granite_json_read(input, strlen(input), &message->input, NULL);
    message->directory = granite_strdup("/p");
    message->model = model;
    return message;
}

/* One call: `sent` said to the runner in order (taken; a NULL closes the host's end there),
 * `answers` given to its asks of the model in order. What the runner said is in `said`, last
 * message last. */
typedef struct transcript {
    granite_exit exit;
    granite_wire *said[16];
    int count;
} transcript;

static void call(granite_wire **sent, int sent_count, bool close_after, const char *const *answers, transcript *t) {
    granite_channel *host, *runner;
    granite_channel_pair(&host, &runner);
    serving s = {runner, GRANITE_EXIT_FAILED};
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &s);
    memset(t, 0, sizeof *t);
    t->said[t->count++] = granite_channel_recv(host); /* the hello */
    for (int i = 0; i < sent_count; i++) {
        granite_channel_send(host, sent[i], NULL);
        granite_wire_free(sent[i]);
    }
    if (close_after) granite_channel_close(host);
    int answered = 0;
    for (;;) {
        granite_wire *message = granite_channel_recv(host);
        if (!message) break;
        if (t->count < 16) t->said[t->count++] = message;
        if (message->kind == GRANITE_WIRE_MODEL && answers && answers[answered]) {
            granite_wire *answer = granite_wire_new(GRANITE_WIRE_ANSWER);
            answer->id = message->id;
            granite_json_read(answers[answered], strlen(answers[answered]), &answer->object, NULL);
            answered++;
            granite_channel_send(host, answer, NULL);
            granite_wire_free(answer);
        }
        if (message->kind == GRANITE_WIRE_OUTPUT || message->kind == GRANITE_WIRE_FAILED) break;
    }
    pthread_join(thread, NULL);
    t->exit = s.exit;
    granite_channel_close(host);
    granite_release(host);
    granite_release(runner);
}

static const granite_wire *last(const transcript *t) {
    return t->said[t->count - 1];
}

static void forget(transcript *t) {
    for (int i = 0; i < t->count; i++) granite_wire_free(t->said[i]);
}

static const char *failed_said(const transcript *t) {
    const granite_wire *message = last(t);
    return message && message->kind == GRANITE_WIRE_FAILED ? message->failure->said : "(not failed)";
}

GRANITE_TEST(a_call_is_answered_once_after_a_hello_that_names_the_answer_standard) {
    transcript t;
    granite_wire *sent[] = {perform("echo", "{\"text\":\"hi\"}", false)};
    call(sent, 1, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_ANSWERED);
    GRANITE_ASSERT(t.said[0] && t.said[0]->kind == GRANITE_WIRE_HELLO);
    GRANITE_ASSERT_STR(t.said[0] ? t.said[0]->wire : NULL, "granite-agent/1");
    GRANITE_ASSERT_STR(t.said[0] ? t.said[0]->answer_standard : NULL, "granite-answer/1");
    GRANITE_ASSERT(last(&t)->kind == GRANITE_WIRE_OUTPUT);
    GRANITE_ASSERT_STR(granite_json_get_string(last(&t)->output, "said"), "hi");
    GRANITE_ASSERT_STR(granite_json_get_string(last(&t)->output, "format"), "granite-answer/1");
    forget(&t);
}

GRANITE_TEST(the_model_is_asked_across_the_channel) {
    transcript t;
    granite_wire *sent[] = {perform("hosted", "{\"ask_model\":true}", true)};
    const char *const answers[] = {"{\"n\":4}", NULL};
    call(sent, 1, false, answers, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_ANSWERED);
    bool noted = false, asked = false;
    for (int i = 0; i < t.count; i++) {
        if (t.said[i]->kind == GRANITE_WIRE_NOTE && strcmp(t.said[i]->note, "offered") == 0) noted = true;
        if (t.said[i]->kind == GRANITE_WIRE_MODEL && t.said[i]->id == 1 && t.said[i]->max_tokens == 3000) asked = true;
    }
    GRANITE_ASSERT(noted && asked);
    GRANITE_ASSERT(last(&t)->kind == GRANITE_WIRE_OUTPUT);
    GRANITE_ASSERT_EQ(json_object_get_int(granite_json_get(last(&t)->output, "n")), 4);
    GRANITE_ASSERT_STR(granite_json_get_string(last(&t)->output, "directory"), "/p");
    forget(&t);
}

GRANITE_TEST(a_call_that_needs_a_model_the_host_does_not_offer_is_refused) {
    transcript t;
    granite_wire *sent[] = {perform("hosted", "{\"ask_model\":true}", false)};
    call(sent, 1, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_REFUSED);
    GRANITE_ASSERT(last(&t)->kind == GRANITE_WIRE_FAILED && last(&t)->failure->kind == GRANITE_REFUSAL_MODEL);
    GRANITE_ASSERT_STR(failed_said(&t), "hosted asks the host's model, and this host has none loaded");
    forget(&t);
}

GRANITE_TEST(a_host_that_leaves_with_an_ask_open_ends_it_without_an_answer) {
    transcript t;
    granite_wire *sent[] = {perform("hosted", "{\"ask_model\":true}", true)};
    call(sent, 1, true, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_REFUSED);
    GRANITE_ASSERT_STR(failed_said(&t), "the host's model gave no answer: the host is gone");
    forget(&t);
}

GRANITE_TEST(input_that_does_not_read_is_refused_as_input_with_exit_three) {
    transcript t;
    granite_wire *sent[] = {perform("echo", "{\"times\":1}", false)};
    call(sent, 1, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_REFUSED);
    GRANITE_ASSERT(last(&t)->kind == GRANITE_WIRE_FAILED && last(&t)->failure->kind == GRANITE_REFUSAL_INPUT);
    GRANITE_ASSERT_STR(failed_said(&t), "echo could not read its input: missing field `text`");
    forget(&t);
}

GRANITE_TEST(an_unknown_word_fails_and_says_what_the_process_carries) {
    transcript t;
    granite_wire *sent[] = {perform("parrot", "{}", false)};
    call(sent, 1, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_FAILED);
    GRANITE_ASSERT_STR(failed_said(&t), "no agent is called `parrot` in this process; it carries `echo`, `hosted`");
    GRANITE_ASSERT(last(&t)->failure->type == GRANITE_ERROR_FAULT);
    forget(&t);
}

GRANITE_TEST(a_cancel_ends_the_call_with_one_thirty) {
    transcript t;
    granite_wire *sent[] = {perform("echo", "{\"text\":\"slow\",\"delay\":5}", false), granite_wire_cancel()};
    call(sent, 2, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_CANCELLED);
    GRANITE_ASSERT_STR(failed_said(&t), "cancelled");
    forget(&t);
    granite_wire *before[] = {granite_wire_cancel()};
    call(before, 1, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_CANCELLED);
    GRANITE_ASSERT_STR(failed_said(&t), "cancelled before the call began");
    forget(&t);
}

GRANITE_TEST(a_host_that_speaks_out_of_turn_or_leaves_ends_the_process_with_two) {
    transcript t;
    granite_wire *note[] = {granite_wire_note("hi")};
    call(note, 1, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_UNREADABLE);
    GRANITE_ASSERT_STR(failed_said(&t), "the host said `note`, which is not a host's message");
    forget(&t);
    call(NULL, 0, true, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_UNREADABLE);
    GRANITE_ASSERT_STR(failed_said(&t), "the host left before making a call");
    forget(&t);
    granite_wire *twice[] = {perform("echo", "{\"text\":\"slow\",\"delay\":5}", false),
                             perform("echo", "{\"text\":\"again\"}", false)};
    call(twice, 2, false, NULL, &t);
    GRANITE_ASSERT_EQ(t.exit, GRANITE_EXIT_UNREADABLE);
    GRANITE_ASSERT_STR(failed_said(&t), "a process answers one call; the host sent a second `perform`");
    forget(&t);
}

int main(void) {
    GRANITE_RUN(a_call_is_answered_once_after_a_hello_that_names_the_answer_standard);
    GRANITE_RUN(the_model_is_asked_across_the_channel);
    GRANITE_RUN(a_call_that_needs_a_model_the_host_does_not_offer_is_refused);
    GRANITE_RUN(a_host_that_leaves_with_an_ask_open_ends_it_without_an_answer);
    GRANITE_RUN(input_that_does_not_read_is_refused_as_input_with_exit_three);
    GRANITE_RUN(an_unknown_word_fails_and_says_what_the_process_carries);
    GRANITE_RUN(a_cancel_ends_the_call_with_one_thirty);
    GRANITE_RUN(a_host_that_speaks_out_of_turn_or_leaves_ends_the_process_with_two);
    GRANITE_TEST_MAIN_END();
}
