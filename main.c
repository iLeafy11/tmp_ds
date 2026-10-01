#include "run.h"   /* first: ctx.h sets the feature macros */
#include "client.h"
#include "path.h"
#include "reply.h"
#include "snapshot.h"
#include "tui.h"
#include "utf8.h"

#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

/* The command line, and the modes that only talk to a daemon (attach, connect, stop, clear, and
 * --forget when a daemon answers). The modes that own an index are in run.c; nothing here touches
 * a search_ctx.
 */

/* Timings. The daemon's first scan can take seconds on a large tree; a client that started it waits
 * this long for the socket before giving up.
 */
#define SNAPSHOT_DEFAULT_SECS 300
#define HALF_LIFE_DEFAULT_DAYS 14
#define DAEMON_START_POLL_MS 100
#define DAEMON_START_TIMEOUT_MS 10000

/* On stdout for help (asked for), on stderr after a mistake. */
static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "Usage: %s DIR                                 search DIR (starts its daemon if needed)\n"
            "       %s query TEXT DIR [--json] [--limit N] [--rescan]\n"
            "       %s stop                                end the daemon (it saves first)\n"
            "       %s forget DIR                          drop what is remembered of your opens\n"
            "       %s clear                               delete every snapshot; stop the daemon first\n"
            "       %s help                                this text (also --help, -h)\n"
            "\n"
            "  DIR alone starts a daemon for DIR if none is running, then opens the TUI on it;\n"
            "  the daemon stays after the TUI exits. query answers from the last snapshot and\n"
            "  scans only when there is none (or with --rescan). forget keeps the index of file\n"
            "  names, only the scores and edges go.\n"
            "\n"
            "For tests and service managers:\n"
            "       %s daemon DIR [--foreground] [--socket PATH] [--snapshot SECS] [--half-life DAYS]\n"
            "       %s standalone DIR [--snapshot SECS] [--half-life DAYS]\n"
            "  daemon is what DIR alone starts for you: it detaches and logs to\n"
            "  $XDG_STATE_HOME/desktop-search/daemon.log; --foreground keeps it on this terminal.\n"
            "  standalone keeps index and TUI in one process and saves on exit; its messages go to\n"
            "  $XDG_STATE_HOME/desktop-search/standalone.log while the TUI runs.\n"
            "  --socket (also for DIR alone, stop, forget, clear) defaults to\n"
            "  $XDG_RUNTIME_DIR/desktop-search.sock. --snapshot and --half-life (also for DIR alone)\n"
            "  set how often a snapshot is saved (default %d s, 0 = only at exit) and over how many\n"
            "  days an access-transition weight halves (default %d, 0 = never).\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, SNAPSHOT_DEFAULT_SECS, HALF_LIFE_DEFAULT_DAYS);
}

/* The socket lives in the per-user runtime directory systemd provides (0700, tmpfs, removed at
 * logout), so only this user can reach the daemon and a reboot leaves no stale file. Without it, a
 * uid-specific name under /tmp.
 */
static bool default_socket_path(char *buf, size_t size)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    int n = dir && dir[0] ? snprintf(buf, size, "%s/desktop-search.sock", dir)
                          : snprintf(buf, size, "/tmp/desktop-search-%d.sock", (int)getuid());
    return n > 0 && (size_t)n < size;
}

/* The whole argument must be the number: atoi and atof read "abc" as 0, which for --snapshot
 * silently meant "never save".
 */
static bool parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno || v < lo || v > hi)
        return false;
    *out = (int)v;
    return true;
}

static bool parse_days(const char *s, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(v) || v < 0)
        return false;
    *out = v;
    return true;
}

/* The command line is a command word, then that command's flags and its directory in any order. A
 * directory alone is the default command. Each command takes only its own flags; the command is
 * known by the time a flag is read, so a flag on the wrong command is refused by name.
 */
enum { FLAG_SOCKET = 1, FLAG_TUNING = 2, FLAG_FOREGROUND = 4, FLAG_QUERY = 8 };

typedef struct command {
    const char *name;   /* "" is the default: DIR alone */
    run_mode mode;
    bool takes_root;
    unsigned flags;
} command;

static const command commands[] = {
    {"",           MODE_ATTACH,     true,  FLAG_SOCKET | FLAG_TUNING},
    {"query",      MODE_QUERY,      true,  FLAG_QUERY},
    {"stop",       MODE_STOP,       false, FLAG_SOCKET},
    {"forget",     MODE_FORGET,     true,  FLAG_SOCKET},
    {"clear",      MODE_CLEAR,      false, FLAG_SOCKET},
    {"daemon",     MODE_DAEMON,     true,  FLAG_SOCKET | FLAG_TUNING | FLAG_FOREGROUND},
    {"standalone", MODE_STANDALONE, true,  FLAG_TUNING},
};

static const char *command_name(const command *cmd)
{
    return cmd->name[0] ? cmd->name : "a directory alone";
}

/* Whether cmd takes the flag; says so when it does not. */
static bool takes(const command *cmd, unsigned flag, const char *arg)
{
    if (cmd->flags & flag)
        return true;
    fprintf(stderr, "%s does not take %s\n", command_name(cmd), arg);
    return false;
}

/* The flag's value, the next argument; NULL (and a message) when there is none. */
static const char *value_of(int argc, char *argv[], int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", argv[*i]);
        return NULL;
    }
    return argv[++*i];
}

static bool parse_options(int argc, char *argv[], options *opt)
{
    *opt = (options){.mode = MODE_ATTACH, .limit = SEARCH_TOP_K, .snapshot_secs = SNAPSHOT_DEFAULT_SECS,
                     .half_life_days = HALF_LIFE_DEFAULT_DAYS};
    const command *cmd = &commands[0];
    int i = 1;
    for (size_t k = 1; argc > 1 && k < sizeof(commands) / sizeof(commands[0]); k++) {
        if (!strcmp(argv[1], commands[k].name)) {
            cmd = &commands[k];
            i = 2;
            break;
        }
    }
    opt->mode = cmd->mode;

    const char *socket_arg = NULL;
    const char *v;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--socket")) {
            if (!takes(cmd, FLAG_SOCKET, a) || !(v = value_of(argc, argv, &i)))
                return false;
            socket_arg = v;
        } else if (!strcmp(a, "--snapshot")) {
            if (!takes(cmd, FLAG_TUNING, a) || !(v = value_of(argc, argv, &i)))
                return false;
            if (!parse_int(v, 0, INT_MAX, &opt->snapshot_secs)) {
                fprintf(stderr, "--snapshot: seconds, 0 to disable\n");
                return false;
            }
            opt->snapshot_given = true;
        } else if (!strcmp(a, "--half-life")) {
            if (!takes(cmd, FLAG_TUNING, a) || !(v = value_of(argc, argv, &i)))
                return false;
            if (!parse_days(v, &opt->half_life_days)) {
                fprintf(stderr, "--half-life: days, 0 to disable\n");
                return false;
            }
            opt->half_life_given = true;
        } else if (!strcmp(a, "--foreground")) {
            if (!takes(cmd, FLAG_FOREGROUND, a))
                return false;
            opt->foreground = true;
        } else if (!strcmp(a, "--json")) {
            if (!takes(cmd, FLAG_QUERY, a))
                return false;
            opt->json = true;
        } else if (!strcmp(a, "--rescan")) {
            if (!takes(cmd, FLAG_QUERY, a))
                return false;
            opt->rescan = true;
        } else if (!strcmp(a, "--limit")) {
            if (!takes(cmd, FLAG_QUERY, a) || !(v = value_of(argc, argv, &i)))
                return false;
            if (!parse_int(v, 1, SEARCH_TOP_K, &opt->limit)) {
                fprintf(stderr, "--limit: 1 to %d\n", SEARCH_TOP_K);
                return false;
            }
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "unknown option %s\n", a);
            return false;
        } else if (cmd->mode == MODE_QUERY && !opt->query) {
            opt->query = a;   /* query TEXT DIR: the text comes first */
        } else if (!cmd->takes_root) {
            fprintf(stderr, "%s takes no directory\n", command_name(cmd));
            return false;
        } else if (opt->root) {
            fprintf(stderr, "one directory only (got %s and %s)\n", opt->root, a);
            return false;
        } else {
            opt->root = a;
            opt->root_argi = i;
        }
    }

    if (cmd->mode == MODE_QUERY && !opt->query) {
        fprintf(stderr, "query needs the text to search for\n");
        return false;
    }
    if (cmd->takes_root && !opt->root) {
        fprintf(stderr, cmd->name[0] ? "%s needs a directory\n" : "a directory is required\n", cmd->name);
        return false;
    }
    if (cmd->flags & FLAG_SOCKET) {
        if (socket_arg)
            snprintf(opt->socket_path, sizeof(opt->socket_path), "%s", socket_arg);
        else if (!default_socket_path(opt->socket_path, sizeof(opt->socket_path))) {
            fprintf(stderr, "socket path too long\n");
            return false;
        }
    }
    return true;
}

static int remote_query(void *arg, const char *text, query_reply *out)
{
    return client_query(arg, text, out);
}

static int remote_status(void *arg, query_reply *out)
{
    return client_status(arg, out);
}

/* Whether a daemon answers on the socket. Fills st when one does. */
static bool daemon_answers(const char *socket_path, query_reply *st)
{
    return !client_status(socket_path, st);
}

/* forget DIR while a daemon answers on the socket (st is its status): the daemon drops its history
 * and saves. main goes to run_with_index instead when none answers, and the snapshot is edited.
 */
static int forget_through_daemon(const options *opt, const char *root, const query_reply *st,
                                 const char *argv0)
{
    if (strcmp(st->root, root)) {
        fprintf(stderr, "The daemon at %s indexes %s, not %s; stop it first (%s stop)\n", opt->socket_path,
                st->root, root, argv0);
        return 1;
    }
    if (client_clear(opt->socket_path)) {
        fprintf(stderr, "%s\n", client_error());
        return 1;
    }
    fprintf(stderr, "The daemon forgot the access history of %s\n", root);
    return 0;
}

/* clear: every snapshot of every root goes, stale ones of renamed or deleted roots included.
 * A running daemon would write its history back at its next snapshot, so it has to be stopped
 * first; only the daemon on this socket can be seen, which is the one the default mode starts.
 */
static int mode_clear(const options *opt, const char *argv0)
{
    static query_reply st;
    if (daemon_answers(opt->socket_path, &st)) {
        fprintf(stderr, "A daemon is running for %s; stop it first (%s stop)\n", st.root, argv0);
        return 1;
    }
    char dir[PATH_MAX];
    if (!snapshot_dir(dir, sizeof(dir))) {
        fprintf(stderr, "cannot find the snapshot directory: no XDG_STATE_HOME or HOME\n");
        return 1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return 1;
    }
    int removed = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        char file[PATH_MAX];
        if (len < 4 || strcmp(e->d_name + len - 4, ".bin") || path_join(file, sizeof(file), dir, e->d_name) < 0)
            continue;
        if (unlink(file))
            perror(file);
        else
            removed++;
    }
    closedir(d);
    fprintf(stderr, "Removed %d snapshot%s from %s\n", removed, removed == 1 ? "" : "s", dir);
    return 0;
}

/* stop: ask the daemon to save and exit. 0 when it acknowledged, 1 when none is running. */
static int mode_stop(const options *opt)
{
    if (client_stop(opt->socket_path)) {
        fprintf(stderr, "%s\n", client_error());
        return 1;
    }
    fprintf(stderr, "Stopped the daemon at %s\n", opt->socket_path);
    return 0;
}

/* The default mode: make sure a daemon for root is running, then hand over to --connect. A daemon
 * that is running but indexes another directory is reported rather than replaced, since there is
 * one socket per user. --snapshot and --half-life go to the daemon this starts; one that is
 * already running keeps its own until it is restarted, and says so.
 *
 * Returns 0 to go on, or an exit status.
 */
static int ensure_daemon(const options *opt, const char *root, const char *argv0)
{
    static query_reply st;
    if (daemon_answers(opt->socket_path, &st)) {
        if (strcmp(st.root, root)) {
            fprintf(stderr, "A daemon is already indexing %s; stop it first (%s stop)\n", st.root, argv0);
            return 1;
        }
        if (opt->snapshot_given)
            fprintf(stderr, "The daemon for %s is already running; --snapshot applies when it next "
                            "starts (%s stop first)\n", root, argv0);
        if (opt->half_life_given)
            fprintf(stderr, "The daemon for %s is already running; --half-life applies when it next "
                            "starts (%s stop first)\n", root, argv0);
        return 0;
    }

    /* Nothing answered: start one. It detaches by itself; wait for its socket to appear. */
    char snapshot_secs[16], half_life[32];
    snprintf(snapshot_secs, sizeof(snapshot_secs), "%d", opt->snapshot_secs);
    snprintf(half_life, sizeof(half_life), "%.17g", opt->half_life_days);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execl("/proc/self/exe", argv0, "daemon", root, "--socket", opt->socket_path, "--snapshot", snapshot_secs,
              "--half-life", half_life, (char *)NULL);
        perror("exec");
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        return 1;   /* it printed why */

    /* The socket exists already (the daemon claims it before detaching), but it is answered only
     * once the snapshot is loaded, which takes seconds for a large one, and each attempt until
     * then waits out the client's own reply timeout. So the limit is measured on the clock, not by
     * counting attempts, which once let a 10-second limit run to minutes.
     */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (daemon_answers(opt->socket_path, &st))
            return 0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long waited_ms = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (waited_ms >= DAEMON_START_TIMEOUT_MS)
            break;
        struct timespec ts = {.tv_nsec = DAEMON_START_POLL_MS * 1000000L};
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "The daemon did not come up on %s (%s)\n", opt->socket_path, client_error());
    return 1;
}

/* DIR alone: a daemon plus the TUI as its client, so the index outlives this session. The TUI has
 * no index of its own; a daemon that goes away meanwhile is reported in its label, not fatal.
 */
static int mode_attach(const options *opt, const char *root, const char *argv0)
{
    int rc = ensure_daemon(opt, root, argv0);
    if (rc)
        return rc;
    tui_backend be = {.arg = (void *)opt->socket_path, .query = remote_query, .status = remote_status,
                      .should_quit = NULL};
    rc = tui_loop(&be) == TUI_OK ? 0 : 1;
    fprintf(stderr, "The daemon keeps indexing %s in the background; %s stop ends it\n", root, argv0);
    return rc;
}

int main(int argc, char *argv[])
{
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout, argv[0]);
        return 0;
    }
    options opt;
    if (!parse_options(argc, argv, &opt)) {
        usage(stderr, argv[0]);
        return 2;
    }

    bsl_init_random();

    /* Writing to a socket whose peer has gone raises SIGPIPE, whose default action is to kill the
     * process: a client that leaves before reading its reply would take the daemon down with it,
     * and a dying daemon would take a client down. Ignored, the write fails with EPIPE instead and
     * the caller reports it. Process-wide policy, so it lives here and not in server.c.
     */
    signal(SIGPIPE, SIG_IGN);

    if (opt.mode == MODE_STOP)
        return mode_stop(&opt);
    if (opt.mode == MODE_CLEAR)
        return mode_clear(&opt, argv[0]);

    /* The index stores paths as given; resolving the root first makes "dir", "./dir" and the
     * absolute form the same index and gives --query callers absolute paths.
     */
    char root[PATH_MAX];
    if (!realpath(opt.root, root)) {
        fprintf(stderr, "%s: %s\n", opt.root, strerror(errno));
        return 2;
    }
    if (!utf8_valid(root)) {   /* every indexed path starts with it, and names are filtered */
        fprintf(stderr, "%s: root path is not valid UTF-8\n", opt.root);
        return 2;
    }

    /* A daemon shows up in ps for a long time and works from /, so its argv should carry the
     * directory as the absolute path it actually uses: re-exec once with the resolved root in place
     * of what was typed ("..", "~/x"). The environment marks the second pass.
     */
    if (opt.mode == MODE_DAEMON && strcmp(opt.root, root) && !getenv("DESKTOP_SEARCH_REEXEC")) {
        setenv("DESKTOP_SEARCH_REEXEC", "1", 1);
        argv[opt.root_argi] = root;
        execv("/proc/self/exe", argv);
        perror("exec");   /* not fatal: carry on with the argv as typed */
    }

    if (opt.mode == MODE_ATTACH)
        return mode_attach(&opt, root, argv[0]);

    /* forget goes to the daemon when one answers; otherwise, like the other index-owning modes,
     * it works on the snapshot itself.
     */
    static query_reply st;
    if (opt.mode == MODE_FORGET && daemon_answers(opt.socket_path, &st))
        return forget_through_daemon(&opt, root, &st, argv[0]);
    return run_with_index(&opt, root);
}
