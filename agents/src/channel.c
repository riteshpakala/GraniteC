/* Where granite-agent/1 messages travel. See granitec/channel.h. */
#include "granitec/channel.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "granitec/buf.h"
#include "granitec/lock.h"
#include "granitec/object.h"
#include "granitec/thread.h"

void granite_trace(const char *arrow, const char *line, size_t len) {
    static int tracing = -1;
    if (tracing < 0) {
        const char *said = getenv("GRANITE_AGENT_TRACE");
        tracing = said && strcmp(said, "1") == 0;
    }
    if (!tracing) return;
    size_t shown = len < 300 ? len : 300;
    while (shown && line[shown - 1] == '\n') shown--;
    fprintf(stderr, "[granite-agent %d] %s %.*s\n", (int)getpid(), arrow, (int)shown, line);
}

typedef struct queued {
    granite_wire *message;
    struct queued *next;
} queued;

struct granite_channel {
    granite_object header;
    /* the reader's side */
    granite_lock lock;
    pthread_cond_t arrived;
    queued *head, *tail;
    bool ended;
    void (*unreadable)(void *arg, const char *line, const char *why);
    void *unreadable_arg;
    /* the writer's side */
    granite_lock writing;
    int write_fd;
};

static void channel_deinit(void *self) {
    granite_channel *channel = self;
    for (queued *q = channel->head, *next; q; q = next) {
        next = q->next;
        granite_wire_free(q->message);
        free(q);
    }
    if (channel->write_fd >= 0) close(channel->write_fd);
    granite_lock_destroy(&channel->lock);
    granite_lock_destroy(&channel->writing);
}

static const granite_class channel_class = {"channel", sizeof(granite_channel), channel_deinit, NULL};

typedef struct reader {
    granite_channel *channel;
    int fd;
} reader;

static void deliver_line(granite_channel *channel, const char *line, size_t len) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\r')) i++;
    if (i == len) return;
    granite_trace("read", line, len);
    char *why = NULL;
    granite_wire *message = granite_wire_read(line, len, &why);
    if (!message) {
        if (channel->unreadable) {
            char shown[201];
            snprintf(shown, sizeof shown, "%.*s", (int)(len < 200 ? len : 200), line);
            channel->unreadable(channel->unreadable_arg, shown, why);
        }
        free(why);
        return;
    }
    queued *q = granite_calloc(1, sizeof *q);
    q->message = message;
    granite_lock_acquire(&channel->lock);
    if (channel->tail) channel->tail->next = q;
    else channel->head = q;
    channel->tail = q;
    pthread_cond_signal(&channel->arrived);
    granite_lock_release(&channel->lock);
}

static void read_lines(void *pointer) {
    reader *r = pointer;
    granite_channel *channel = r->channel;
    granite_buf pending = {0};
    char *chunk = granite_malloc(1 << 16);
    for (;;) {
        ssize_t n = read(r->fd, chunk, 1 << 16);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        granite_buf_append(&pending, chunk, (size_t)n);
        size_t start = 0;
        for (size_t i = 0; i < pending.len; i++) {
            if (pending.data[i] != '\n') continue;
            deliver_line(channel, pending.data + start, i - start);
            start = i + 1;
        }
        if (start) {
            memmove(pending.data, pending.data + start, pending.len - start);
            pending.len -= start;
            pending.data[pending.len] = '\0';
        }
    }
    free(chunk);
    granite_buf_free(&pending);
    close(r->fd);
    granite_lock_acquire(&channel->lock);
    channel->ended = true;
    pthread_cond_broadcast(&channel->arrived);
    granite_lock_release(&channel->lock);
    granite_release(channel);
    free(r);
}

granite_channel *granite_pipe_channel_new(int read_fd, int write_fd,
                                          void (*unreadable)(void *arg, const char *line, const char *why), void *arg) {
    granite_channel *channel = granite_object_new(&channel_class);
    granite_lock_init(&channel->lock, "a channel's messages");
    granite_lock_init(&channel->writing, "a channel's writer");
    pthread_cond_init(&channel->arrived, NULL);
    channel->unreadable = unreadable;
    channel->unreadable_arg = arg;
    channel->write_fd = write_fd;
    reader *r = granite_calloc(1, sizeof *r);
    r->channel = granite_retain(channel);
    r->fd = read_fd;
    if (!granite_thread_spawn("granite.channel", 0, read_lines, r)) {
        perror("granite: the channel's reader could not start");
        abort();
    }
    return channel;
}

static void cloexec_pipe(int fds[2]) {
    if (pipe(fds) != 0) {
        perror("granite: no pipe");
        abort();
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
}

void granite_channel_pair(granite_channel **a, granite_channel **b) {
    int there[2], back[2];
    cloexec_pipe(there);
    cloexec_pipe(back);
    *a = granite_pipe_channel_new(back[0], there[1], NULL, NULL);
    *b = granite_pipe_channel_new(there[0], back[1], NULL, NULL);
}

bool granite_channel_send(granite_channel *channel, const granite_wire *message, char **error) {
    size_t len;
    char *line = granite_wire_line(message, &len);
    granite_trace("sent", line, len);
    granite_lock_acquire(&channel->writing);
    int fd = channel->write_fd;
    int failed = fd < 0 ? -1 : 0;
    for (size_t written = 0; !failed && written < len;) {
        ssize_t n = write(fd, line + written, len - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) failed = errno ? errno : EIO;
        else written += (size_t)n;
    }
    granite_lock_release(&channel->writing);
    free(line);
    if (failed && error) *error = failed < 0 ? granite_strdup("the channel is closed") : granite_strdup(strerror(failed));
    return !failed;
}

granite_wire *granite_channel_recv(granite_channel *channel) {
    granite_lock_acquire(&channel->lock);
    while (!channel->head && !channel->ended) granite_lock_wait(&channel->lock, &channel->arrived);
    queued *q = channel->head;
    if (q) {
        channel->head = q->next;
        if (!channel->head) channel->tail = NULL;
    }
    granite_lock_release(&channel->lock);
    if (!q) return NULL;
    granite_wire *message = q->message;
    free(q);
    return message;
}

void granite_channel_close(granite_channel *channel) {
    granite_lock_acquire(&channel->writing);
    int fd = channel->write_fd;
    channel->write_fd = -1;
    granite_lock_release(&channel->writing);
    if (fd >= 0) close(fd);
}
