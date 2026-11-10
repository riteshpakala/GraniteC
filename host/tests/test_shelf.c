/* The host's records: digests, where code comes from, installation.json, and a shelf read the way
 * GraniteRust's AgentShelf reads one. */
#include <sys/stat.h>
#include <unistd.h>

#include "granite_host/files.h"
#include "granite_host/sha256.h"
#include "granite_host/shelf.h"
#include "granite_test.h"

GRANITE_TEST(sha256_is_fips_180_4) {
    unsigned char digest[GRANITE_SHA256_BYTES];
    char hex[65];
    granite_sha256("abc", 3, digest);
    granite_sha256_hex(digest, hex);
    GRANITE_ASSERT_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    granite_sha256("", 0, digest);
    granite_sha256_hex(digest, hex);
    GRANITE_ASSERT_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void expect_source(const char *said, const char *path, const char *url, const char *revision) {
    granite_source source = granite_source_parse(said, "/work/here");
    if ((path || source.path) && (!path || !source.path || strcmp(path, source.path) != 0))
        GRANITE_FAIL("%s: path %s", said, source.path ? source.path : "(none)");
    if ((url || source.url) && (!url || !source.url || strcmp(url, source.url) != 0))
        GRANITE_FAIL("%s: url %s", said, source.url ? source.url : "(none)");
    if ((revision || source.revision) && (!revision || !source.revision || strcmp(revision, source.revision) != 0))
        GRANITE_FAIL("%s: revision %s", said, source.revision ? source.revision : "(none)");
    granite_source_free(&source);
}

GRANITE_TEST(a_source_is_a_repository_at_a_revision_or_a_folder) {
    expect_source("https://github.com/rao/ComicAgent.git", NULL, "https://github.com/rao/ComicAgent.git", NULL);
    expect_source(" https://github.com/rao/ComicAgent.git@f9bdda7 ", NULL, "https://github.com/rao/ComicAgent.git", "f9bdda7");
    expect_source("git@github.com:rao/ComicAgent.git", NULL, "git@github.com:rao/ComicAgent.git", NULL);
    expect_source("/abs/agent/../agent", "/abs/agent", NULL, NULL);
    expect_source("./agent", "/work/here/agent", NULL, NULL);
    expect_source("../agent", "/work/agent", NULL, NULL);
    char home[512];
    snprintf(home, sizeof home, "%s/agents/comic", getenv("HOME"));
    expect_source("~/agents/comic", home, NULL, NULL);
    granite_source pinned = granite_source_parse("https://x/y.git@abc", "/");
    granite_source newest = granite_source_newest(&pinned);
    char *text = granite_source_text(&newest);
    GRANITE_ASSERT_STR(text, "https://x/y.git");
    free(text);
    text = granite_source_text(&pinned);
    GRANITE_ASSERT_STR(text, "https://x/y.git@abc");
    free(text);
    granite_source_free(&pinned);
    granite_source_free(&newest);
}

static void write_text(const char *path, const char *text) {
    GRANITE_ASSERT(granite_file_write(path, text, strlen(text)));
}

/* An installation folder `name` on `shelf`, its runner `bytes`, its record written. */
static void install(const char *shelf, const char *name, const char *word, const char *bytes) {
    char *folder = granite_path_join(shelf, name);
    granite_mkdirs(folder);
    char *runner = granite_path_join(folder, "agent");
    write_text(runner, bytes);
    granite_installation record = {0};
    record.format = GRANITE_INSTALLATION_FORMAT;
    record.name = (char *)name;
    record.source.path = (char *)"/src/agent";
    record.granite = (char *)"0.1.0@abc";
    record.executable = (char *)"agent";
    char digest[65];
    granite_sha256_file(runner, digest);
    record.sha256 = digest;
    granite_stamp_of(runner, &record.size, &record.modified);
    record.has_modified = true;
    record.built_at = (char *)"2026-11-09T12:00:00Z";
    char *entries = granite_strdupf("[{\"kind\":\"package\",\"word\":\"%s\",\"summary\":\"s\",\"module\":\"M\",\"type\":\"M.T\"}]", word);
    granite_json_read(entries, strlen(entries), &record.entries, NULL);
    json_object *json = granite_installation_json(&record);
    char *text = granite_json_write_pretty(json, NULL);
    char *manifest = granite_path_join(folder, GRANITE_INSTALLATION_FILE);
    write_text(manifest, text);
    granite_json_put(json);
    granite_json_put(record.entries);
    free(text);
    free(manifest);
    free(entries);
    free(runner);
    free(folder);
}

GRANITE_TEST(an_installation_record_reads_back_as_it_was_written) {
    granite_installation record = {0};
    record.name = (char *)"comic";
    record.source.url = (char *)"https://x/comic.git";
    record.revision = (char *)"f9bdda7";
    record.executable = (char *)"agent";
    record.sha256 = (char *)"00";
    record.size = 7;
    record.built_at = (char *)"2026-11-09T12:00:00Z";
    json_object *json = granite_installation_json(&record);
    char *text = granite_json_write(json, NULL);
    GRANITE_ASSERT_STR(text, "{\"builtAt\":\"2026-11-09T12:00:00Z\",\"entries\":[],\"executable\":\"agent\","
                             "\"format\":\"granite-agent-installation/1\",\"name\":\"comic\",\"revision\":\"f9bdda7\","
                             "\"sha256\":\"00\",\"size\":7,\"source\":{\"url\":\"https://x/comic.git\"}}");
    char *error = NULL;
    granite_installation *back = granite_installation_read(json, &error);
    GRANITE_ASSERT(back != NULL);
    GRANITE_ASSERT_STR(back ? back->source.url : NULL, "https://x/comic.git");
    GRANITE_ASSERT(back && !back->has_modified && back->size == 7);
    granite_installation_free(back);
    json_object_object_del(json, "sha256");
    GRANITE_ASSERT(granite_installation_read(json, &error) == NULL);
    GRANITE_ASSERT_STR(error, "missing field `sha256`");
    free(error);
    free(text);
    granite_json_put(json);
}

GRANITE_TEST(a_shelf_reads_its_installations_and_says_what_does_not_read) {
    char *shelf = granite_temporary_folder("shelf");
    install(shelf, "Comic", "comic", "runner one");
    install(shelf, "plate", "plate", "runner two");
    char *broken = granite_path_join(shelf, "broken"), *later = granite_path_join(shelf, "later");
    char *hidden = granite_path_join(shelf, ".Comic.new"), *empty = granite_path_join(shelf, "empty");
    granite_mkdirs(broken);
    granite_mkdirs(later);
    granite_mkdirs(hidden);
    granite_mkdirs(empty);
    char *path = granite_path_join(broken, GRANITE_INSTALLATION_FILE);
    write_text(path, "{\"format\":");
    free(path);
    path = granite_path_join(later, GRANITE_INSTALLATION_FILE);
    write_text(path, "{\"format\":\"granite-agent-installation/9\",\"name\":\"later\",\"source\":{\"path\":\"/x\"},"
                     "\"executable\":\"agent\",\"sha256\":\"0\",\"size\":0,\"builtAt\":\"x\",\"entries\":[]}");
    free(path);

    granite_shelf_read read = granite_shelf_read_all(shelf);
    GRANITE_ASSERT_EQ(read.count, 2);
    GRANITE_ASSERT_STR(read.count ? read.installations[0]->name : NULL, "Comic");
    GRANITE_ASSERT_EQ(read.problem_count, 2);
    GRANITE_ASSERT_STR(read.problem_count ? read.problems[0] : NULL,
                       "broken: its installation.json does not read: EOF while parsing a value at line 1 column 10");
    GRANITE_ASSERT_STR(read.problem_count > 1 ? read.problems[1] : NULL,
                       "later: granite-agent-installation/9 is not an installation this host reads");
    granite_shelf_read_free(&read);

    granite_installation *by_name = granite_shelf_find(shelf, "comic");
    granite_installation *by_word = granite_shelf_find(shelf, "PLATE");
    GRANITE_ASSERT(by_name && strcmp(by_name->name, "Comic") == 0);
    GRANITE_ASSERT(by_word && strcmp(by_word->name, "plate") == 0);
    GRANITE_ASSERT(granite_shelf_find(shelf, "nobody") == NULL);

    GRANITE_ASSERT(granite_shelf_verify(shelf, by_name) == NULL);
    char *runner = granite_shelf_executable_of(shelf, by_name);
    write_text(runner, "runner one, changed");
    granite_remote_error *changed = granite_shelf_verify(shelf, by_name);
    GRANITE_ASSERT(changed != NULL);
    char *said = changed ? granite_remote_text(changed) : NULL;
    char *expected = granite_strdupf("comic's executable changed since it was installed (%s); install it again", runner);
    GRANITE_ASSERT_STR(said, expected);
    char *stamp = granite_shelf_stamp(shelf);
    GRANITE_ASSERT(strstr(stamp, "Comic@") != NULL && strstr(stamp, "plate@") != NULL);
    free(stamp);
    free(said);
    free(expected);
    free(runner);
    granite_remote_free(changed);
    granite_installation_free(by_name);
    granite_installation_free(by_word);
    granite_remove_tree(shelf);
    GRANITE_ASSERT(!granite_exists(shelf));
    free(shelf);
    free(broken);
    free(later);
    free(hidden);
    free(empty);
}

GRANITE_TEST(each_remote_error_is_said_as_rust_says_it) {
    granite_remote_error *ended = granite_remote_new(GRANITE_REMOTE_ENDED, "parrot");
    ended->status = granite_strdup("signal 6 (SIGABRT)");
    ended->log = granite_strdup("this line must not reach the wire");
    char *text = granite_remote_text(ended);
    GRANITE_ASSERT_STR(text, "parrot's process ended (signal 6 (SIGABRT)) without answering: this line must not reach the wire");
    free(text);
    granite_remote_error *refused = granite_remote_new(GRANITE_REMOTE_REFUSED, "parrot");
    refused->message = granite_strdup("told to fail");
    refused->refusal = granite_refuse(GRANITE_REFUSAL_CALL, "told to fail: leave out `fail`");
    text = granite_remote_text(refused);
    GRANITE_ASSERT_STR(text, "parrot refused (call): told to fail");
    free(text);
    granite_remote_free(ended);
    granite_remote_free(refused);
}

int main(void) {
    GRANITE_RUN(sha256_is_fips_180_4);
    GRANITE_RUN(a_source_is_a_repository_at_a_revision_or_a_folder);
    GRANITE_RUN(an_installation_record_reads_back_as_it_was_written);
    GRANITE_RUN(a_shelf_reads_its_installations_and_says_what_does_not_read);
    GRANITE_RUN(each_remote_error_is_said_as_rust_says_it);
    GRANITE_TEST_MAIN_END();
}
