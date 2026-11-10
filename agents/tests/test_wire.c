/* The messages of granite-agent/1, as lines: every line in agents/tests/wire/corpus.hex read as
 * GraniteRust's AgentWire::read reads it (rust.expected, from scripts/wire-goldens.sh), and written
 * back to the same bytes. */
#include "granite_test.h"
#include "granitec/wire.h"

static char *read_file(const char *relative, size_t *len) {
    char path[1024];
    granite_test_path(relative, path, sizeof path);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    granite_buf buf = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, file)) > 0) granite_buf_append(&buf, chunk, n);
    fclose(file);
    *len = buf.len;
    return granite_buf_take(&buf);
}

static size_t unhex(const char *hex, size_t n, char *out) {
    size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned value;
        sscanf(hex + i, "%2x", &value);
        out[k++] = (char)value;
    }
    out[k] = '\0';
    return k;
}

GRANITE_TEST(every_line_reads_and_writes_back_as_graniterust_does) {
    size_t corpus_len = 0, expected_len = 0;
    char *corpus = read_file("agents/tests/wire/corpus.hex", &corpus_len);
    char *expected = read_file("agents/tests/wire/rust.expected", &expected_len);
    GRANITE_ASSERT(corpus && expected);
    if (!corpus || !expected) return;
    char *case_save = NULL, *expected_save = NULL;
    char *case_line = strtok_r(corpus, "\n", &case_save);
    char *expected_line = strtok_r(expected, "\n", &expected_save);
    int lines = 0;
    char line[8192], want[8192];
    while (case_line && expected_line) {
        lines++;
        size_t n = unhex(case_line, strlen(case_line), line);
        char *error = NULL;
        granite_wire *message = granite_wire_read(line, n, &error);
        if (strncmp(expected_line, "ok ", 3) == 0) {
            size_t want_len = unhex(expected_line + 3, strlen(expected_line + 3), want);
            if (!message) {
                GRANITE_FAIL("%s: Rust read it, GraniteC said: %s", line, error);
            } else {
                size_t got_len;
                char *got = granite_wire_line(message, &got_len);
                if (got_len != want_len || memcmp(got, want, got_len) != 0)
                    GRANITE_FAIL("%s: wrote %.*s, Rust wrote %s", line, (int)got_len - 1, got, want);
                free(got);
            }
        } else {
            unhex(expected_line + 4, strlen(expected_line + 4), want);
            if (message) GRANITE_FAIL("%s: Rust said \"%s\", GraniteC read it", line, want);
            else if (strcmp(error, want) != 0) GRANITE_FAIL("%s: said \"%s\", Rust said \"%s\"", line, error, want);
        }
        granite_wire_free(message);
        free(error);
        case_line = strtok_r(NULL, "\n", &case_save);
        expected_line = strtok_r(NULL, "\n", &expected_save);
    }
    GRANITE_ASSERT(lines > 70);
    free(corpus);
    free(expected);
}

GRANITE_TEST(hello_is_the_golden_line) {
    granite_wire *hello = granite_wire_hello(7, false);
    char *line = granite_wire_line(hello, NULL);
    GRANITE_ASSERT_STR(line, "{\"hello\":{\"pid\":7,\"wire\":\"granite-agent/1\"}}\n");
    free(line);
    granite_wire_free(hello);
    hello = granite_wire_hello(4512, true);
    line = granite_wire_line(hello, NULL);
    GRANITE_ASSERT_STR(line, "{\"hello\":{\"answer\":\"granite-answer/1\",\"pid\":4512,\"wire\":\"granite-agent/1\"}}\n");
    free(line);
    granite_wire_free(hello);
}

GRANITE_TEST(a_refusal_crosses_the_wire_beside_its_sentence_and_a_cancel_without_one) {
    const char *reasons[] = {"`post` is mapped twice", "`Comment` has no `text`"};
    granite_error *refusal = granite_refuse_said(GRANITE_REFUSAL_CALL, "2 problems with the call", reasons, 2);
    granite_wire *failed = granite_wire_failed(refusal);
    char *line = granite_wire_line(failed, NULL);
    GRANITE_ASSERT_STR(line, "{\"failed\":\"2 problems with the call\",\"refused\":{\"kind\":\"call\",\"reasons\":"
                             "[\"`post` is mapped twice\",\"`Comment` has no `text`\"]}}\n");
    char *error = NULL;
    granite_wire *back = granite_wire_read(line, strlen(line), &error);
    GRANITE_ASSERT(back && back->kind == GRANITE_WIRE_FAILED);
    GRANITE_ASSERT(back && back->failure->type == GRANITE_ERROR_REFUSED && back->failure->reason_count == 2);
    free(line);
    granite_wire_free(back);
    granite_wire_free(failed);
    granite_error_free(refusal);

    granite_error *cancelled = granite_cancelled();
    granite_wire *said = granite_wire_failed(cancelled);
    line = granite_wire_line(said, NULL);
    GRANITE_ASSERT_STR(line, "{\"failed\":\"cancelled\"}\n");
    free(line);
    granite_wire_free(said);
    granite_error_free(cancelled);

    granite_wire *cancel = granite_wire_cancel();
    line = granite_wire_line(cancel, NULL);
    GRANITE_ASSERT_STR(line, "{\"cancel\":{}}\n");
    free(line);
    granite_wire_free(cancel);
}

GRANITE_TEST(a_model_ask_says_whole_seconds_as_integers) {
    granite_wire *ask = granite_wire_new(GRANITE_WIRE_MODEL);
    ask->id = 1;
    ask->ask_kind = GRANITE_ASK_OBJECT;
    granite_json_read("{\"type\":\"object\"}", 17, &ask->schema, NULL);
    ask->system = granite_strdup("s");
    ask->user = granite_strdup("u");
    ask->max_tokens = 3000;
    ask->seconds = 300.0;
    char *line = granite_wire_line(ask, NULL);
    GRANITE_ASSERT_STR(line, "{\"model\":{\"id\":1,\"kind\":\"object\",\"maxTokens\":3000,\"schema\":{\"type\":\"object\"},"
                             "\"seconds\":300,\"system\":\"s\",\"user\":\"u\"}}\n");
    free(line);
    granite_wire_free(ask);
}

int main(void) {
    GRANITE_RUN(every_line_reads_and_writes_back_as_graniterust_does);
    GRANITE_RUN(hello_is_the_golden_line);
    GRANITE_RUN(a_refusal_crosses_the_wire_beside_its_sentence_and_a_cancel_without_one);
    GRANITE_RUN(a_model_ask_says_whole_seconds_as_integers);
    GRANITE_TEST_MAIN_END();
}
