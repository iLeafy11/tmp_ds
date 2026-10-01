#include "client.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Why the last call failed. Nothing here prints: a TUI polling a daemon that is gone would get a
 * line over its screen four times a second, so the caller decides whether the reason is shown.
 */
static char last_error[PATH_MAX + 128];

/* A macro, so each use is a direct snprintf call and the compiler checks its format against its
 * arguments.
 */
#define set_error(...) snprintf(last_error, sizeof(last_error), __VA_ARGS__)

const char *client_error(void)
{
    return last_error;
}

/* Reads until the server closes the connection; the reply is one line and the server closes right
 * after it. Bounded by the largest reply the server can produce.
 */
static int read_all(int fd, char *buf, size_t size)
{
    size_t total = 0;
    for (;;) {
        if (total >= size - 1) {
            set_error("client: reply too long");
            return -1;
        }
        ssize_t n = read(fd, buf + total, size - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                set_error("client: the daemon accepted the connection but did not answer in time "
                          "(stopped with Ctrl+Z, or busy?)");
            else
                set_error("client: read: %s", strerror(errno));
            return -1;
        }
        if (!n)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (int)total;
}

/* How long to wait for the daemon to answer; must exceed the daemon's own per-client timeout
 * (CLIENT_IO_TIMEOUT_SECS in server.c) so that the daemon always gives up first.
 */
#define DAEMON_REPLY_TIMEOUT_SECS 2

/* One round trip: connect, send verb (and argument) as one line, read until the server closes.
 * reply must hold the largest reply the server can produce.
 *
 * Returns the reply length or -1.
 */
static int round_trip(const char *sock_path, const char *verb, const char *arg, char *reply, size_t size)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        set_error("client: socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path) >= (int)sizeof(addr.sun_path)) {
        set_error("client: socket path too long");
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = DAEMON_REPLY_TIMEOUT_SECS};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int rc = -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        set_error("client: %s: %s (is the daemon running?)", sock_path, strerror(errno));
        goto out;
    }

    size_t vlen = strlen(verb), alen = arg ? strlen(arg) : 0;
    size_t len = vlen + (arg ? 1 + alen : 0) + 1;
    char *line = malloc(len + 1);
    if (!line) {
        set_error("client: out of memory");
        goto out;
    }
    memcpy(line, verb, vlen);
    size_t at = vlen;
    if (arg) {
        line[at++] = ' ';
        memcpy(line + at, arg, alen);
        at += alen;
    }
    line[at++] = '\n';
    line[at] = '\0';
    ssize_t w = write(fd, line, len);
    free(line);
    if (w != (ssize_t)len) {
        set_error("client: write: %s", w < 0 ? strerror(errno) : "short write");
        goto out;
    }
    rc = read_all(fd, reply, size);

out:
    close(fd);
    return rc;
}

/* Room for SEARCH_TOP_K paths of PATH_MAX, each possibly escaped, plus the status fields. */
static char reply_buf[SEARCH_TOP_K * PATH_MAX * 6 + 4096];

/* A verb that is answered with a full reply line. */
static int ask_reply(const char *sock_path, const char *verb, const char *arg, query_reply *out)
{
    if (round_trip(sock_path, verb, arg, reply_buf, sizeof(reply_buf)) < 0)
        return -1;
    if (reply_parse(reply_buf, out)) {
        set_error("client: malformed reply");
        return -1;
    }
    return 0;
}

int client_query(const char *sock_path, const char *text, query_reply *out)
{
    return ask_reply(sock_path, "query", text, out);
}

int client_status(const char *sock_path, query_reply *out)
{
    return ask_reply(sock_path, "status", NULL, out);
}

/* A verb that is answered with {"ok":true} and nothing else. */
static int ask_ok(const char *sock_path, const char *verb)
{
    char reply[64];
    if (round_trip(sock_path, verb, NULL, reply, sizeof(reply)) < 0)
        return -1;
    if (strncmp(reply, "{\"ok\":true}", 11)) {
        set_error("client: the daemon did not acknowledge %s: %.*s", verb, (int)strcspn(reply, "\n"), reply);
        return -1;
    }
    return 0;
}

int client_clear(const char *sock_path) { return ask_ok(sock_path, "clear"); }
int client_stop(const char *sock_path)  { return ask_ok(sock_path, "stop"); }
