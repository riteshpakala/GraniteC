/* A C agent repository made a runner on this machine: fetched, read, built, asked what it carries,
 * and installed on a shelf with its record. GraniteRust's AgentInstaller, for C repositories:
 *
 *     fetch → granite-agent.json → make O=<work>/build <runner> → <runner> --describe → install
 *
 * PIN: THE REPOSITORY SHIPS ITS RUNNER. granite-agent.json names it ({"name", "runner",
 * "granite"}); `make O=<folder> <runner>` builds it, finding GraniteC through pkg-config alone.
 * Installed, it is always called `agent`.
 * PIN: ONE WORD, ONE INSTALLATION. A repository that carries a word another installation carries
 * is refused; installing the same one again replaces it whole.
 * PIN: THE STAGES ARE SWIFT'S WORDS: fetching, reading the package, building, asking … what it
 * carries, installing, installed. */
#ifndef GRANITE_HOST_INSTALL_H
#define GRANITE_HOST_INSTALL_H

#include "granite_host/installation.h"

#define GRANITE_AGENT_MANIFEST "granite-agent.json"

typedef enum granite_install_type {
    GRANITE_INSTALL_TOOLCHAIN,
    GRANITE_INSTALL_FETCH,
    GRANITE_INSTALL_NOT_A_PACKAGE,
    GRANITE_INSTALL_NOT_GRANITE,
    GRANITE_INSTALL_BUILD,
    GRANITE_INSTALL_NO_AGENT,
    GRANITE_INSTALL_CLASH,
    GRANITE_INSTALL_NOT_INSTALLED,
} granite_install_type;

typedef struct granite_install_error {
    granite_install_type type;
    char *source; /* fetch, not a package */
    char *reason; /* toolchain, fetch, not a package, not granite */
    char *name;   /* not granite, build, no agent, not installed */
    char *log;    /* build: its last lines */
    char *path;   /* build: where the whole log is */
    char *word;   /* clash */
    char *installed;
} granite_install_error;

char *granite_install_text(const granite_install_error *error);
void granite_install_error_free(granite_install_error *error);

/* Told of each stage, and of each line a build prints (`line` NULL for a stage). */
typedef void (*granite_progress)(void *arg, const char *stage, const char *line);

/* Installs what `source` holds on the shelf at `folder`. NULL with the record in `*installed`
 * (optional; the caller frees it), or why it was not installed. */
granite_install_error *granite_install(const char *folder, const granite_source *source, granite_progress progress,
                                       void *arg, granite_installation **installed);
/* Installs the installation called `name` again, from its source at its newest. */
granite_install_error *granite_install_update(const char *folder, const char *name, granite_progress progress, void *arg,
                                              granite_installation **installed);
/* Removes the installation called `name`; `*removed` (optional) is its record. */
granite_install_error *granite_install_remove(const char *folder, const char *name, granite_installation **removed);

/* A command's end: its status (127 when it was not found), its stdout, and all it printed. */
typedef struct granite_command_said {
    int status;
    char *stdout_text;
    char *output;
} granite_command_said;

/* Runs `arguments` in `folder` with stdin empty and the environment without DYLD_* and the make
 * flags of whatever make runs us; each line it prints to `line` (optional). */
granite_command_said granite_command_run(const char *const *arguments, const char *folder,
                                         void (*line)(void *arg, const char *text), void *arg);
void granite_command_said_free(granite_command_said *said);

#endif
