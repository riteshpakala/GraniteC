/* JSON read and written as serde_json reads and writes it: every case in agents/tests/json/corpus.hex
 * against what serde_json 1.0.154 made of it (serde.expected, from scripts/json-goldens.sh). */
#include "granite_test.h"
#include "granitec/json.h"
#include "granitec/lock.h"

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

/* The next line of `text` (without its newline), from `*at`; NULL at the end. */
static char *next_line(char *text, size_t len, size_t *at) {
    if (*at >= len) return NULL;
    char *line = text + *at;
    char *end = memchr(line, '\n', len - *at);
    if (end) {
        *end = '\0';
        *at = (size_t)(end - text) + 1;
    } else {
        *at = len;
    }
    return line;
}

static size_t unhex(const char *hex, size_t n, char *out) {
    size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned value;
        sscanf(hex + i, "%2x", &value);
        out[k++] = (char)value;
    }
    return k;
}

static void printable(const char *bytes, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 5 < cap; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c >= 0x20 && c < 0x7f) out[k++] = (char)c;
        else k += (size_t)snprintf(out + k, cap - k, "\\x%02x", c);
    }
    out[k] = '\0';
}

GRANITE_TEST(every_case_reads_and_writes_as_serde_json_does) {
    size_t corpus_len = 0, expected_len = 0;
    char *corpus = read_file("agents/tests/json/corpus.hex", &corpus_len);
    char *expected = read_file("agents/tests/json/serde.expected", &expected_len);
    GRANITE_ASSERT(corpus && expected);
    if (!corpus || !expected) return;
    size_t at_case = 0, at_expected = 0;
    int cases = 0, mismatches = 0;
    char *case_line, *expected_line;
    char *input = granite_malloc(corpus_len + 1), *want = granite_malloc(expected_len + 1);
    char *want_pretty = granite_malloc(expected_len + 1);
    while ((case_line = next_line(corpus, corpus_len, &at_case))) {
        expected_line = next_line(expected, expected_len, &at_expected);
        if (!expected_line) break;
        cases++;
        size_t input_len = unhex(case_line, strlen(case_line), input);
        json_object *value = NULL;
        char *error = NULL;
        bool read = granite_json_read(input, input_len, &value, &error);
        char got[4096] = "", wanted[4096] = "", shown[512];
        printable(input, input_len < 60 ? input_len : 60, shown, sizeof shown);
        if (strncmp(expected_line, "ok ", 3) == 0) {
            char *space = strchr(expected_line + 3, ' ');
            size_t want_len = unhex(expected_line + 3, (size_t)(space - expected_line - 3), want);
            size_t pretty_len = unhex(space + 1, strlen(space + 1), want_pretty);
            if (!read) {
                if (mismatches++ < 12) GRANITE_FAIL("case %d (%s): serde read it, GraniteC said: %s", cases, shown, error);
            } else {
                size_t n, pn;
                char *compact = granite_json_write(value, &n);
                char *pretty = granite_json_write_pretty(value, &pn);
                if (n != want_len || memcmp(compact, want, n) != 0) {
                    printable(compact, n < 200 ? n : 200, got, sizeof got);
                    printable(want, want_len < 200 ? want_len : 200, wanted, sizeof wanted);
                    if (mismatches++ < 12) GRANITE_FAIL("case %d (%s): wrote %s, serde wrote %s", cases, shown, got, wanted);
                } else if (pn != pretty_len || memcmp(pretty, want_pretty, pn) != 0) {
                    printable(pretty, pn < 200 ? pn : 200, got, sizeof got);
                    printable(want_pretty, pretty_len < 200 ? pretty_len : 200, wanted, sizeof wanted);
                    if (mismatches++ < 12) GRANITE_FAIL("case %d (%s): indented %s, serde %s", cases, shown, got, wanted);
                }
                free(compact);
                free(pretty);
            }
        } else {
            size_t want_len = unhex(expected_line + 4, strlen(expected_line + 4), want);
            want[want_len] = '\0';
            if (read) {
                if (mismatches++ < 12) GRANITE_FAIL("case %d (%s): serde said \"%s\", GraniteC read it", cases, shown, want);
            } else if (strcmp(error, want) != 0) {
                if (mismatches++ < 12) GRANITE_FAIL("case %d (%s): said \"%s\", serde said \"%s\"", cases, shown, error, want);
            }
        }
        granite_json_put(value);
        free(error);
    }
    if (mismatches) GRANITE_FAIL("%d of %d cases differ from serde_json", mismatches, cases);
    GRANITE_ASSERT(cases > 900);
    free(input);
    free(want);
    free(want_pretty);
    free(corpus);
    free(expected);
}

GRANITE_TEST(a_whole_double_is_an_integer_as_swift_writes_it) {
    struct {
        double value;
        const char *written;
    } cases[] = {{300, "300"}, {300.5, "300.5"}, {-2, "-2"}, {0, "0"}, {9007199254740992.0, "9007199254740992.0"},
                 {1e20, "1e+20"}, {0.25, "0.25"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        json_object *number = granite_json_number(cases[i].value);
        char *written = granite_json_write(number, NULL);
        GRANITE_ASSERT_STR(written, cases[i].written);
        free(written);
        granite_json_put(number);
    }
}

GRANITE_TEST(equality_is_by_value_and_keys) {
    json_object *a = NULL, *b = NULL, *c = NULL;
    GRANITE_ASSERT(granite_json_read("{\"x\":[1,2.0,\"s\"],\"y\":null}", 26, &a, NULL));
    GRANITE_ASSERT(granite_json_read("{\"y\":null,\"x\":[1.0,2,\"s\"]}", 26, &b, NULL));
    GRANITE_ASSERT(granite_json_read("{\"y\":null,\"x\":[1.0,2,\"t\"]}", 26, &c, NULL));
    GRANITE_ASSERT(granite_json_equal(a, b));
    GRANITE_ASSERT(!granite_json_equal(a, c));
    json_object *copy = granite_json_copy(a);
    GRANITE_ASSERT(granite_json_equal(copy, a));
    GRANITE_ASSERT(granite_json_has(a, "y") && granite_json_get(a, "y") == NULL);
    GRANITE_ASSERT(!granite_json_has(a, "z"));
    granite_json_put(a);
    granite_json_put(b);
    granite_json_put(c);
    granite_json_put(copy);
}

static granite_lock some_lock;

static void frees_under_a_lock(void) {
    json_object *value = json_object_new_object();
    granite_lock_acquire(&some_lock);
    granite_json_put(value);
}

GRANITE_TEST(a_tree_is_never_freed_under_a_lock) {
    GRANITE_ASSERT(granite_test_aborts(frees_under_a_lock, "freed a JSON value while holding some lock"));
}

int main(void) {
    granite_lock_init(&some_lock, "some lock");
    GRANITE_RUN(every_case_reads_and_writes_as_serde_json_does);
    GRANITE_RUN(a_whole_double_is_an_integer_as_swift_writes_it);
    GRANITE_RUN(equality_is_by_value_and_keys);
    GRANITE_RUN(a_tree_is_never_freed_under_a_lock);
    GRANITE_TEST_MAIN_END();
}
