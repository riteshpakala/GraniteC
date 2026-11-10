/* What an input takes, and granite-answer/1: a declared schema in Rust's data form and as JSON
 * Schema, values checked against it in serde's words, and each way an answer breaks the standard
 * said as Swift's AgentAnswerCheck says it. */
#include <sys/stat.h>

#include "granite_test.h"
#include "granitec/answer.h"
#include "granitec/schema.h"

static const char *const moods[] = {"calm", "loud", "sly", NULL};
static const granite_schema tag = {GRANITE_SCHEMA_STRING, NULL, NULL, NULL};
static const granite_field card_fields[] = {
    {"name", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, true, "who the card is for"},
    {"count", {GRANITE_SCHEMA_INTEGER, NULL, NULL, NULL}, false, NULL},
    {"mood", {GRANITE_SCHEMA_CHOICE, moods, NULL, NULL}, true, NULL},
    {"tags", {GRANITE_SCHEMA_ARRAY, NULL, &tag, NULL}, false, NULL},
    {"directory", {GRANITE_SCHEMA_STRING, NULL, NULL, NULL}, false, NULL},
    {NULL, {0}, false, NULL},
};
static const granite_schema card = {GRANITE_SCHEMA_OBJECT, NULL, NULL, card_fields};

static char *written(json_object *value) {
    char *text = granite_json_write(value, NULL);
    granite_json_put(value);
    return text;
}

GRANITE_TEST(a_schema_is_written_as_rust_writes_its_data_form) {
    char *value = written(granite_schema_value(&card));
    GRANITE_ASSERT_STR(value,
                       "{\"object\":[{\"name\":\"name\",\"required\":true,\"schema\":\"string\"},"
                       "{\"name\":\"count\",\"required\":false,\"schema\":\"integer\"},"
                       "{\"name\":\"mood\",\"required\":true,\"schema\":{\"choice\":[\"calm\",\"loud\",\"sly\"]}},"
                       "{\"name\":\"tags\",\"required\":false,\"schema\":{\"array\":\"string\"}},"
                       "{\"name\":\"directory\",\"required\":false,\"schema\":\"string\"}]}");
    free(value);
    char *any = written(granite_schema_value(NULL));
    GRANITE_ASSERT_STR(any, "\"any\"");
    free(any);
    char *notes = written(granite_schema_notes(&card));
    GRANITE_ASSERT_STR(notes, "{\"name\":\"who the card is for\"}");
    free(notes);
    GRANITE_ASSERT(granite_schema_takes_directory(&card));
}

GRANITE_TEST(a_model_is_shown_json_schema_without_the_hosts_folder) {
    char *shown = written(granite_schema_json_schema(&card, true));
    GRANITE_ASSERT_STR(shown,
                       "{\"properties\":{\"count\":{\"type\":\"integer\"},\"mood\":{\"enum\":[\"calm\",\"loud\",\"sly\"],"
                       "\"type\":\"string\"},\"name\":{\"description\":\"who the card is for\",\"type\":\"string\"},"
                       "\"tags\":{\"items\":{\"type\":\"string\"},\"type\":\"array\"}},\"required\":[\"name\",\"mood\"],"
                       "\"type\":\"object\"}");
    free(shown);
}

static char *checked(const char *json) {
    json_object *value = NULL;
    if (!granite_json_read(json, strlen(json), &value, NULL)) return granite_strdup("(did not read)");
    char *said = granite_schema_check(&card, value);
    granite_json_put(value);
    return said;
}

GRANITE_TEST(a_value_that_does_not_read_is_said_in_serdes_words) {
    struct {
        const char *input, *said;
    } rows[] = {
        {"{\"name\":\"ada\",\"mood\":\"calm\"}", NULL},
        {"{\"name\":\"ada\",\"mood\":\"calm\",\"count\":null,\"extra\":[1]}", NULL},
        {"{\"mood\":\"calm\"}", "missing field `name`"},
        {"{\"name\":\"ada\"}", "missing field `mood`"},
        {"{\"name\":5,\"mood\":\"calm\"}", "invalid type: integer `5`, expected a string"},
        {"{\"name\":\"ada\",\"mood\":\"grim\"}", "unknown variant `grim`, expected one of `calm`, `loud`, `sly`"},
        {"{\"name\":\"ada\",\"mood\":\"calm\",\"count\":1.5}", "invalid type: floating point `1.5`, expected an integer"},
        {"{\"name\":\"ada\",\"mood\":\"calm\",\"tags\":[\"a\",2]}", "invalid type: integer `2`, expected a string"},
        {"{\"name\":\"ada\",\"mood\":\"calm\",\"tags\":\"a\"}", "invalid type: string \"a\", expected a sequence"},
        {"[1]", "invalid type: sequence, expected an object"},
        {"{\"count\":true}", "invalid type: boolean `true`, expected an integer"},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char *said = checked(rows[i].input);
        if (rows[i].said == NULL) {
            if (said) GRANITE_FAIL("%s: said \"%s\"", rows[i].input, said);
        } else if (!said || strcmp(said, rows[i].said) != 0) {
            GRANITE_FAIL("%s: said \"%s\", expected \"%s\"", rows[i].input, said ? said : "(nothing)", rows[i].said);
        }
        free(said);
    }
}

static const char *const card_into[] = {"cards/", NULL};
static const granite_make makes[] = {{"card", "a card", card_into, NULL}, {NULL, NULL, NULL, NULL}};

static char *joined_problems(json_object *answer, const char *folder) {
    size_t count = 0;
    char **problems = granite_answer_problems(answer, "maker", makes, folder, &count);
    granite_buf text = {0};
    for (size_t i = 0; i < count; i++) {
        granite_buf_appendf(&text, "%s%s", i ? " | " : "", problems[i]);
        free(problems[i]);
    }
    free(problems);
    return granite_buf_take(&text);
}

static void make_folder(char *folder, size_t n) {
    snprintf(folder, n, "/tmp/granitec-answer-XXXXXX");
    if (!mkdtemp(folder)) GRANITE_FAIL("no temporary folder");
}

GRANITE_TEST(a_conforming_answer_has_no_problems_and_is_stamped) {
    char folder[64];
    make_folder(folder, sizeof folder);
    json_object *answer = granite_answer_new();
    granite_answer_path(answer, "cards/ada.card.json");
    granite_answer_made(answer, "card", "ada", "cards/ada.card.json", NULL);
    granite_answer_note(answer, "%d card made", 1);
    granite_json_set_string(answer, "card", "ada");
    char *problems = joined_problems(answer, folder);
    GRANITE_ASSERT_STR(problems, "");
    free(problems);
    json_object *stamped = granite_answer_stamped(answer, "maker");
    char *line = written(stamped);
    GRANITE_ASSERT_STR(line, "{\"agent\":{\"word\":\"maker\"},\"card\":\"ada\",\"expected\":[],\"format\":\"granite-answer/1\","
                             "\"made\":[{\"kind\":\"card\",\"name\":\"ada\",\"path\":\"cards/ada.card.json\",\"state\":\"written\"}],"
                             "\"notes\":[\"1 card made\"],\"path\":\"cards/ada.card.json\",\"paths\":[\"cards/ada.card.json\"],"
                             "\"payloads\":[],\"problems\":[]}");
    free(line);
    char *empty = written(granite_answer_new());
    GRANITE_ASSERT_STR(empty, "{\"expected\":[],\"made\":[],\"path\":null,\"paths\":[],\"payloads\":[],\"problems\":[]}");
    free(empty);
    granite_json_put(answer);
}

GRANITE_TEST(each_way_to_break_the_standard_is_said_as_swift_says_it) {
    char folder[64], file[128];
    make_folder(folder, sizeof folder);
    snprintf(file, sizeof file, "%s/cards", folder);
    mkdir(file, 0755);
    snprintf(file, sizeof file, "%s/cards/ada.card.json", folder);
    FILE *payload = fopen(file, "w");
    fputs("{\"schema\":\"other-card/1\"}", payload);
    fclose(payload);

    json_object *answer = granite_answer_new();
    granite_answer_add_path(answer, "cards/ada.card.json");
    granite_json_set_string(answer, "path", "cards/elsewhere.card.json");
    granite_answer_made(answer, "chart", "ada", "cards/ada.card.json", NULL);
    granite_answer_made(answer, "card", "", NULL, NULL);
    granite_answer_made(answer, "card", "bob", NULL, "rewritten");
    granite_answer_made(answer, "card", "cy", "cards/ada.card.json", "torn");
    granite_answer_expected(answer, "poster", "x");
    granite_answer_problem(answer, "Partly Done", "x");
    granite_answer_payload(answer, "cards/ada.card.json", "test-card/1");
    granite_answer_payload(answer, "cards/none.json", "test-card/1");
    granite_json_set_string(answer, "format", "granite-answer/1");
    granite_json_set(answer, "notes", json_object_new_string("one"));
    char *problems = joined_problems(answer, folder);
    GRANITE_ASSERT_STR(problems,
                       "`format` is the runner's to say: an answer does not encode it"
                       " | `path` cards/elsewhere.card.json is not one of `paths`"
                       " | `made[0]` is a `chart`, and maker makes `card`"
                       " | `made[1]` needs a `kind` and a `name`"
                       " | `made[2]` was written, so it names its `path`"
                       " | `made[3].state` is one of `written`, `rewritten`, `unchanged`, `removed`"
                       " | `expected[0]` is a `poster`, and maker makes `card`"
                       " | `problems[0].kind` is one lowercase word"
                       " | `payloads[0]` says `test-card/1`, and the file's own `schema` is `other-card/1`"
                       " | `payloads[1]` cards/none.json is not one of `paths`"
                       " | `notes` is a list of sentences");
    free(problems);
    granite_json_put(answer);

    json_object *bare = json_object_new_object();
    granite_json_set(bare, "made", json_object_new_int(1));
    problems = joined_problems(bare, folder);
    GRANITE_ASSERT_STR(problems, "`paths` is missing: an answer names every file it wrote, `[]` when none | `made` is not a list");
    free(problems);
    granite_json_put(bare);
}

GRANITE_TEST(paths_are_compared_as_files) {
    char folder[64], absolute[128];
    make_folder(folder, sizeof folder);
    snprintf(absolute, sizeof absolute, "%s/cards/a.json", folder);
    char *relative = granite_answer_resolved("cards/a.json", folder);
    char *full = granite_answer_resolved(absolute, NULL);
    char *wandering = granite_answer_resolved("./cards/../cards/a.json", folder);
    GRANITE_ASSERT_STR(relative, full);
    GRANITE_ASSERT_STR(wandering, full);
#if defined(__APPLE__)
    char *tmp = granite_answer_resolved("/tmp/x/y", NULL);
    GRANITE_ASSERT_STR(tmp, "/private/tmp/x/y");
    free(tmp);
#endif
    char *root = granite_answer_resolved("/../..", NULL);
    GRANITE_ASSERT_STR(root, "/");
    free(root);
    free(relative);
    free(full);
    free(wandering);
}

int main(void) {
    GRANITE_RUN(a_schema_is_written_as_rust_writes_its_data_form);
    GRANITE_RUN(a_model_is_shown_json_schema_without_the_hosts_folder);
    GRANITE_RUN(a_value_that_does_not_read_is_said_in_serdes_words);
    GRANITE_RUN(a_conforming_answer_has_no_problems_and_is_stamped);
    GRANITE_RUN(each_way_to_break_the_standard_is_said_as_swift_says_it);
    GRANITE_RUN(paths_are_compared_as_files);
    GRANITE_TEST_MAIN_END();
}
