/* `granite check`'s validator against GraniteRust's: every case in jsonschema/tests/check/cases.jsonl
 * said as Rust's `granite check` says it (rust.expected, from scripts/check-goldens.sh); and a census
 * that fails when a schema uses a keyword the validator does not read. */
#include "granite_host/files.h"
#include "granite_jsonschema/validate.h"
#include "granite_test.h"

static granite_schemas *registry(void) {
    granite_schemas *schemas = granite_schemas_new();
    char folder[1024];
    char *error = NULL;
    granite_test_path("Schema", folder, sizeof folder);
    GRANITE_ASSERT(granite_schemas_add_folder(schemas, folder, &error));
    granite_test_path("containers/schemas", folder, sizeof folder);
    GRANITE_ASSERT(granite_schemas_add_folder(schemas, folder, &error));
    free(error);
    return schemas;
}

/* What the Rust command prints to stderr for one case, and its exit code. */
static char *as_rust_says(const granite_schemas *schemas, const char *name, const json_object *instance, int *code) {
    const json_object *schema = granite_schemas_find(schemas, name);
    if (!schema) {
        *code = 2;
        return granite_strdupf("granite: %s: no such schema\n", name);
    }
    size_t count = 0;
    char **problems = granite_schema_validate(schemas, schema, instance, &count);
    granite_buf said = {0};
    if (!count) granite_buf_appendf(&said, "valid: instance.json against %s\n", name);
    for (size_t i = 0; i < count; i++) {
        granite_buf_appendf(&said, "%s\n", problems[i]);
        free(problems[i]);
    }
    if (count) granite_buf_appendf(&said, "granite: instance.json does not keep %s: %zu problems\n", name, count);
    free(problems);
    *code = count ? 1 : 0;
    return granite_buf_take(&said);
}

GRANITE_TEST(every_case_is_said_as_graniterusts_check_says_it) {
    char path[1024];
    size_t cases_len = 0, expected_len = 0;
    char *cases = granite_file_read(granite_test_path("jsonschema/tests/check/cases.jsonl", path, sizeof path), &cases_len);
    char *expected = granite_file_read(granite_test_path("jsonschema/tests/check/rust.expected", path, sizeof path), &expected_len);
    GRANITE_ASSERT(cases && expected);
    if (!cases || !expected) return;
    granite_schemas *schemas = registry();
    char *case_save = NULL, *expected_save = NULL;
    int seen = 0;
    for (char *c = strtok_r(cases, "\n", &case_save), *e = strtok_r(expected, "\n", &expected_save); c && e;
         c = strtok_r(NULL, "\n", &case_save), e = strtok_r(NULL, "\n", &expected_save)) {
        seen++;
        json_object *one = NULL, *want = NULL;
        granite_json_read(c, strlen(c), &one, NULL);
        granite_json_read(e, strlen(e), &want, NULL);
        int code = 0;
        const char *name = granite_json_get_string(one, "schema");
        char *said = as_rust_says(schemas, name, granite_json_get(one, "instance"), &code);
        const char *rust = granite_json_get_string(want, "stderr");
        int rust_code = json_object_get_int(granite_json_get(want, "code"));
        if (code != rust_code || strcmp(said, rust) != 0)
            GRANITE_FAIL("case %d against %s:\n  GraniteC (%d): %s  Rust (%d): %s", seen, name, code, said, rust_code, rust);
        free(said);
        granite_json_put(one);
        granite_json_put(want);
    }
    GRANITE_ASSERT(seen > 50);
    granite_schemas_free(schemas);
    free(cases);
    free(expected);
}

static bool known(const char *keyword) {
    for (const char *const *k = granite_jsonschema_keywords; *k; k++)
        if (strcmp(*k, keyword) == 0) return true;
    return false;
}

/* Every keyword of a schema node: the names under properties and $defs are names, not keywords;
 * enum, const and examples hold values. */
static void census(const json_object *node, const char *where, int *unknown) {
    json_type type = json_object_get_type((json_object *)node);
    if (type == json_type_array) {
        for (size_t i = 0; i < json_object_array_length((json_object *)node); i++)
            census(json_object_array_get_idx((json_object *)node, i), where, unknown);
        return;
    }
    if (type != json_type_object) return;
    json_object_object_foreach((json_object *)node, key, value) {
        if (!known(key)) {
            GRANITE_FAIL("%s uses `%s`, which granite check does not read", where, key);
            (*unknown)++;
        }
        if (strcmp(key, "properties") == 0 || strcmp(key, "$defs") == 0) {
            json_object_object_foreach(value, name, member) {
                (void)name;
                census(member, where, unknown);
            }
        } else if (strcmp(key, "enum") != 0 && strcmp(key, "const") != 0 && strcmp(key, "examples") != 0) {
            census(value, where, unknown);
        }
    }
}

GRANITE_TEST(every_schema_uses_only_keywords_the_validator_reads) {
    const char *files[] = {"Schema/granite-agent-wire.schema.json", "Schema/granite-agent-entry.schema.json",
                           "Schema/granite-agent-answer.schema.json", "Schema/granite-agent-installation.schema.json",
                           "containers/schemas/craft-agent-output.schema.json", NULL};
    int unknown = 0;
    for (const char **file = files; *file; file++) {
        char path[1024];
        size_t len = 0;
        char *bytes = granite_file_read(granite_test_path(*file, path, sizeof path), &len);
        json_object *schema = NULL;
        GRANITE_ASSERT(bytes && granite_json_read(bytes, len, &schema, NULL));
        census(schema, *file, &unknown);
        granite_json_put(schema);
        free(bytes);
    }
    GRANITE_ASSERT_EQ(unknown, 0);
}

GRANITE_TEST(additional_properties_and_false_schemas_are_said_as_jsonschema_rs_says_them) {
    granite_schemas *schemas = granite_schemas_new();
    json_object *schema = NULL, *instance = NULL;
    const char *text = "{\"properties\":{\"a\":{}},\"additionalProperties\":false}";
    granite_json_read(text, strlen(text), &schema, NULL);
    granite_json_read("{\"a\":1,\"c\":2,\"b\":3}", 19, &instance, NULL);
    size_t count = 0;
    char **problems = granite_schema_validate(schemas, schema, instance, &count);
    GRANITE_ASSERT_EQ(count, 1);
    GRANITE_ASSERT_STR(count ? problems[0] : NULL, "Additional properties are not allowed ('b', 'c' were unexpected) at ");
    for (size_t i = 0; i < count; i++) free(problems[i]);
    free(problems);
    granite_json_put(schema);
    granite_json_put(instance);
    granite_schemas_free(schemas);
}

int main(void) {
    GRANITE_RUN(every_case_is_said_as_graniterusts_check_says_it);
    GRANITE_RUN(every_schema_uses_only_keywords_the_validator_reads);
    GRANITE_RUN(additional_properties_and_false_schemas_are_said_as_jsonschema_rs_says_them);
    GRANITE_TEST_MAIN_END();
}
