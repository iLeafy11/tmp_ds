#include "server.h"   /* first: ctx.h sets the feature macros the system headers below need */
#include "reply.h"
#include "snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void trim_newline(char *str)
{
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r'))
        str[--len] = '\0';
}

/* The open lock file whose flock says a daemon owns the socket path; -1 when none is held. */
static int socket_lock_fd = -1;

/* Takes the flock on "<path>.lock". Returns false, with the reason on stderr, when another instance
 * holds it or the lock file cannot be opened.
 */
static bool acquire_socket_lock(const char *path)
{
    char lock_path[PATH_MAX];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >= (int)sizeof(lock_path)) {
        fprintf(stderr, "server_listen: path too long: %s\n", path);
        return false;
    }
    socket_lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (socket_lock_fd < 0) {
        perror(lock_path);
        return false;
    }
    if (flock(socket_lock_fd, LOCK_EX | LOCK_NB)) {
        if (errno == EWOULDBLOCK)
            fprintf(stderr, "already running at %s\n", path);
        else
            perror("flock");
        close(socket_lock_fd);
        socket_lock_fd = -1;
        return false;
    }
    return true;
}

/* Releases the flock by closing the lock file. The lock file itself stays: removing it would let
 * two instances starting at once each lock a different file under the same name.
 */
static void release_socket_lock(void)
{
    if (socket_lock_fd >= 0) {
        close(socket_lock_fd);
        socket_lock_fd = -1;
    }
}

int server_listen(const char *path)
{
    if (!path || !path[0])
        return -1;

    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "server_listen: path too long: %s\n", path);
        return -1;
    }

    if (!acquire_socket_lock(path))
        return -1;
    /* A socket there was left by an instance that no longer holds the lock. Anything else is not
     * ours: a mistyped --socket must not delete a file.
     */
    struct stat st;
    if (!lstat(path, &st)) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a socket\n", path);
            release_socket_lock();
            return -1;
        }
        unlink(path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        release_socket_lock();
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        release_socket_lock();
        return -1;
    }

    if (listen(fd, 8) < 0) {
        perror("listen");
        server_close(fd, path);
        return -1;
    }

    return fd;
}

/* How long a connected client may take to send its whole line, and how long each write of the
 * reply may block. The client side (client.c) waits longer than this, so a slow daemon is never
 * mistaken for a silent one.
 */
#define CLIENT_IO_TIMEOUT_SECS 1

/* Reads one line (the request: a verb, then maybe an argument) into buf. The client fd is blocking
 * with a one-second timeout per read, and the line as a whole has the same deadline: a client that
 * says nothing, or drips one byte at a time to stay under the per-read timeout, is dropped within
 * about two seconds.
 *
 * Returns the line length, or -1.
 */
static int read_request(int client_fd, char *buf, size_t size)
{
    struct timeval tv = {.tv_sec = CLIENT_IO_TIMEOUT_SECS};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    size_t total = 0;
    for (;;) {
        if (total >= size - 1) {
            fprintf(stderr, "server: request longer than %zu bytes (fd=%d)\n", size - 1, client_fd);
            return -1;
        }
        ssize_t n = read(client_fd, buf + total, size - 1 - total);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (n > 0 && (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9 > CLIENT_IO_TIMEOUT_SECS &&
            !memchr(buf + total, '\n', (size_t)n)) {
            fprintf(stderr, "server: client did not finish its line within the timeout (fd=%d)\n", client_fd);
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                fprintf(stderr, "server: client sent nothing within the timeout (fd=%d)\n", client_fd);
            else
                perror("server: read");
            return -1;
        }
        if (!n) {
            /* Closed without a newline: take what came. Closed without anything is not a request at
             * all (the liveness probe of a second instance does exactly this).
             */
            if (!total)
                return -1;
            break;
        }
        total += (size_t)n;
        if (memchr(buf + total - (size_t)n, '\n', (size_t)n))
            break;
    }
    buf[total] = '\0';
    trim_newline(buf);
    return (int)total;
}

int server_serve_one(search_ctx *ctx, int client_fd)
{
    char buf[4096];

    if (read_request(client_fd, buf, sizeof(buf)) < 0)
        goto fail;

    /* From here the FILE owns the fd: fclose closes it, so the fail: label must not. */
    FILE *out = fdopen(client_fd, "w");
    if (!out) {
        perror("server: fdopen");
        goto fail;
    }

    /* The reply is large (up to SEARCH_TOP_K paths), so it lives in static storage rather than on
     * the event loop's stack; the loop serves one client at a time.
     */
    static query_reply reply;
    if (!strncmp(buf, "query ", 6)) {
        reply_fill(ctx, buf + 6, &reply);
        reply_write(out, &reply);
    } else if (!strcmp(buf, "status")) {
        reply_fill_status(ctx, &reply);
        reply_write(out, &reply);
    } else if (!strcmp(buf, "clear")) {
        ctx_forget(ctx);
        snapshot_save_if_changed(ctx);
        fputs("{\"ok\":true}\n", out);
    } else if (!strcmp(buf, "stop")) {
        fputs("{\"ok\":true}\n", out);
        ctx->shutdown = true;
    } else {
        fputs("{\"error\":\"unknown request\"}\n", out);
    }
    if (fclose(out) == EOF)
        perror("server: write");
    return 0;

fail:
    close(client_fd);
    return -1;
}

void server_close(int listen_fd, const char *path)
{
    if (listen_fd >= 0)
        close(listen_fd);
    if (path)
        unlink(path);
    release_socket_lock();
}
