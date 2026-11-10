/* What calling an agent in another process can say. See granite_host/remote.h. */
#include "granite_host/remote.h"

#include <stdlib.h>

#include "granitec/buf.h"

granite_remote_error *granite_remote_new(granite_remote_type type, const char *word) {
    granite_remote_error *error = granite_calloc(1, sizeof *error);
    error->type = type;
    error->word = granite_strdup(word ? word : "");
    return error;
}

char *granite_remote_text(const granite_remote_error *e) {
    switch (e->type) {
    case GRANITE_REMOTE_FAILED: return granite_strdupf("%s failed: %s", e->word, e->message);
    case GRANITE_REMOTE_REFUSED:
        return granite_strdupf("%s refused (%s): %s", e->word,
                               granite_refusal_kind_name(e->refusal ? e->refusal->kind : GRANITE_REFUSAL_HOST), e->message);
    case GRANITE_REMOTE_FAULT: return granite_strdupf("%s failed by its own fault: %s", e->word, e->message);
    case GRANITE_REMOTE_ENDED:
        if (e->log && *e->log) return granite_strdupf("%s's process ended (%s) without answering: %s", e->word, e->status, e->log);
        return granite_strdupf("%s's process ended (%s) without answering", e->word, e->status);
    case GRANITE_REMOTE_UNREADABLE: return granite_strdupf("%s's process broke the wire: %s", e->word, e->reason);
    case GRANITE_REMOTE_CHANGED:
        return granite_strdupf("%s's executable changed since it was installed (%s); install it again", e->word, e->path);
    case GRANITE_REMOTE_SPAWN: return granite_strdupf("could not start %s: %s", e->path, e->reason);
    }
    return granite_strdup("");
}

void granite_remote_free(granite_remote_error *e) {
    if (!e) return;
    free(e->word);
    free(e->message);
    granite_error_free(e->refusal);
    free(e->status);
    free(e->log);
    free(e->reason);
    free(e->path);
    free(e);
}
