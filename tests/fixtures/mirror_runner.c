/* `mirror`: answers with its input, as the runner read it and writes it back. What interop holds to
 * serde: every value of the JSON corpus through a C runner's reader and writer, against what
 * serde_json makes of the same text. Test-only. */
#include <granitec/granitec.h>
#include <granitec/server.h>

static granite_error *mirror_run(granite_context *context, const json_object *input) {
    json_object *answer = granite_answer_new();
    granite_json_set(answer, "mirrored", granite_json_copy(granite_json_get(input, "value")));
    granite_deliver(context, answer);
    return NULL;
}

static const granite_package mirror = {
    .word = "mirror",
    .summary = "answers with what it was given.",
    .module = "granite_interop",
    .run = mirror_run,
};

static const granite_package *const roster[] = {&mirror, NULL};

int main(int argc, char **argv) {
    return granite_server_main(argc, argv, roster);
}
