/* A folder of installed runners, one folder each with its installation.json: what a host reads to
 * offer the agents they carry, without running one. GraniteRust's AgentShelf.
 *
 * PIN: A CHANGED EXECUTABLE IS NOT RUN. An executable whose digest is not the one recorded at
 * install is refused with a sentence; installing it again is the way back. The digest is taken only
 * when the size or modification time moved.
 * PIN: ONLY `agents add` WRITES TO A SHELF. Everything here reads. */
#ifndef GRANITE_HOST_SHELF_H
#define GRANITE_HOST_SHELF_H

#include <stddef.h>

#include "granite_host/installation.h"
#include "granite_host/remote.h"

typedef struct granite_shelf_read {
    granite_installation **installations; /* by name */
    size_t count;
    char **problems; /* a sentence for each folder whose installation does not read */
    size_t problem_count;
} granite_shelf_read;

/* Every installation on the shelf at `folder`; one that does not read is left out and said. */
granite_shelf_read granite_shelf_read_all(const char *folder);
void granite_shelf_read_free(granite_shelf_read *read);
/* The installation called `name`, or carrying the agent `name`, in any case; NULL when none. */
granite_installation *granite_shelf_find(const char *folder, const char *name);
/* The installation's folder, and its executable. The caller frees them. */
char *granite_shelf_folder_of(const char *folder, const granite_installation *installation);
char *granite_shelf_executable_of(const char *folder, const granite_installation *installation);
/* What changes when an installation is added, removed or rewritten. The caller frees it. */
char *granite_shelf_stamp(const char *folder);
/* NULL when the executable is the one installed, else a `changed` error naming it. */
granite_remote_error *granite_shelf_verify(const char *folder, const granite_installation *installation);

#endif
