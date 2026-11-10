/* One installed runner, as installation.json keeps it beside the executable, and where its code
 * came from. GraniteRust's AgentInstallation and AgentSource: the same record, so a C host reads a
 * Rust host's shelf and the other way around. */
#ifndef GRANITE_HOST_INSTALLATION_H
#define GRANITE_HOST_INSTALLATION_H

#include <stdbool.h>
#include <stdint.h>

#include "granitec/json.h"

#define GRANITE_INSTALLATION_FORMAT "granite-agent-installation/1"
#define GRANITE_INSTALLATION_FILE "installation.json"

/* A folder (`path`), or a git repository (`url`, at `revision` or its default branch's head). */
typedef struct granite_source {
    char *path;
    char *url;
    char *revision;
} granite_source;

/* `https://…/ComicAgent.git`, `…/ComicAgent.git@f9bdda7`, `git@github.com:rao/ComicAgent.git`, or a
 * folder (`/abs`, `~/x`, `./x`, `../x`, resolved against `base`). */
granite_source granite_source_parse(const char *said, const char *base);
/* The same source at its newest: a repository's head, a folder as it is. */
granite_source granite_source_newest(const granite_source *source);
/* `url@revision`, `url` or `path`. The caller frees it. */
char *granite_source_text(const granite_source *source);
json_object *granite_source_json(const granite_source *source);
void granite_source_free(granite_source *source);

typedef struct granite_installation {
    char *format;
    char *name;
    granite_source source;
    char *revision;   /* the source's commit, for a repository */
    char *granite;    /* what the runner was built against: version, and @commit from git */
    char *executable; /* relative to the installation's folder */
    char *sha256;
    uint64_t size;
    bool has_modified;
    double modified; /* the executable's modification time when installed */
    char *built_at;
    json_object *entries;
} granite_installation;

/* The record as installation.json writes it (camelCase, absent fields left out). */
json_object *granite_installation_json(const granite_installation *installation);
/* The record `value` holds, or NULL with why in `*error`. */
granite_installation *granite_installation_read(const json_object *value, char **error);
/* The first agent's word, or the name when it carries none. */
const char *granite_installation_word(const granite_installation *installation);
void granite_installation_free(granite_installation *installation);

#endif
