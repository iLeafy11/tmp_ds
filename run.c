#include "run.h"   /* first: ctx.h sets the feature macros */
#include "path.h"
#include "reply.h"
#include "scan.h"
#include "server.h"
#include "snapshot.h"
#include "threadpool.h"
#include "tui.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Everything a mode that owns an index goes through, from creating the context to the final save:
 * the event loop and the PageRank thread (the two background threads that belong to no other
 * subsystem; the scan thread is scan.c's), the start and stop of all three, the daemon's
 * detaching, the standalone log, and the two modes (--query, --forget) that only read the
 * snapshot. main.c holds the command line and the modes that only talk to a daemon; it never
 * touches a search_ctx.
 */

/* The event loop: one epoll over everything that happens to a running index. inotify (changes to
 * the tree), the listening socket (daemon mode), the snapshot timer, the shutdown signals and a
 * wake-up eventfd. What is in the epoll is decided here and nowhere else.
 */

static void shutdown_sigset(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGHUP);
}

/* Blocks SIGINT, SIGTERM and SIGHUP, so that they reach the loop through the signalfd instead of
 * killing the process. Must run before any thread exists (ctx_create starts the pool), since each
 * thread inherits the mask it is created with.
 */
static int event_block_signals(void)
{
    sigset_t set;
    shutdown_sigset(&set);
    int err = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (err) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(err));
        return -1;
    }
    return 0;
}

static int epoll_add(int epoll_fd, int fd, const char *what)
{
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev)) {
        fprintf(stderr, "epoll_ctl (%s): %s\n", what, strerror(errno));
        return -1;
    }
    return 0;
}

/* The timer is armed only when seconds > 0; 0 means save at exit only. */
static int init_timer(search_ctx *ctx, int seconds)
{
    if (seconds <= 0)
        return 0;
    ctx->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ctx->timer_fd == -1) {
        perror("timerfd_create");
        return -1;
    }
    struct itimerspec spec = {
        .it_interval = {.tv_sec = seconds},
        .it_value = {.tv_sec = seconds},
    };
    if (timerfd_settime(ctx->timer_fd, 0, &spec, NULL) == -1) {
        perror("timerfd_settime");
        return -1;
    }
    return epoll_add(ctx->epoll_fd, ctx->timer_fd, "timer");
}

/* Creates the descriptors and registers them all: epoll, inotify (watch_open), the wake-up
 * eventfd, the signalfd, a listen_fd set beforehand (daemon mode), and a timer that ticks every
 * snapshot_secs (0: none, save only at exit).
 */
static int event_init(search_ctx *ctx, int snapshot_secs)
{
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd == -1) {
        perror("epoll_create1");
        return -1;
    }
    if (watch_open(&ctx->ws) || epoll_add(ctx->epoll_fd, ctx->ws.inotify_fd, "inotify"))
        return -1;

    ctx->shutdown_fd = eventfd(0, EFD_NONBLOCK);
    if (ctx->shutdown_fd == -1) {
        perror("eventfd");
        return -1;
    }
    if (epoll_add(ctx->epoll_fd, ctx->shutdown_fd, "shutdown"))
        return -1;

    sigset_t set;
    shutdown_sigset(&set);
    ctx->signal_fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (ctx->signal_fd == -1) {
        perror("signalfd");
        return -1;
    }
    if (epoll_add(ctx->epoll_fd, ctx->signal_fd, "signal"))
        return -1;

    /* Daemon mode: main claimed the listening socket before creating the context. */
    if (ctx->listen_fd >= 0 && epoll_add(ctx->epoll_fd, ctx->listen_fd, "listen"))
        return -1;

    return init_timer(ctx, snapshot_secs);
}

/* Wakes the loop so that it returns. The caller sets ctx->shutdown first. */
static void event_wake(search_ctx *ctx)
{
    uint64_t wake = 1;
    if (write(ctx->shutdown_fd, &wake, sizeof(wake)) == -1 && errno != EAGAIN)
        perror("write (shutdown)");
}

/* Closes what event_init created, except the inotify fd, which watch_destroy closes. */
static void event_close(search_ctx *ctx)
{
    int *fds[] = {&ctx->epoll_fd, &ctx->shutdown_fd, &ctx->signal_fd, &ctx->timer_fd};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (*fds[i] >= 0)
            close(*fds[i]);
        *fds[i] = -1;
    }
}

/* One snapshot tick: decays the edge weights if an hour has passed since the last decay, then
 * saves if anything changed since the last save. On this thread, which is also the only writer of
 * last_accessed_pr_id, so the snapshot is consistent without extra locking there.
 */
static void snapshot_tick(search_ctx *ctx)
{
    uint64_t ticks;
    if (read(ctx->timer_fd, &ticks, sizeof(ticks)) != sizeof(ticks))
        return;
    ctx_decay(ctx, time(NULL));
    snapshot_save_if_changed(ctx);
}

/* Readiness on the listening socket means at least one client is queued, possibly several, so
 * accept until the queue is empty (EAGAIN). Each request is answered right here, synchronously: a
 * query takes microseconds to a millisecond, and server_serve_one bounds a silent client with a
 * timeout, so the inotify side never waits long.
 */
static void accept_clients(search_ctx *ctx)
{
    for (;;) {
        int cfd = accept4(ctx->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;             /* interrupted, or the client gave up while queued */
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept");
            return;
        }
        server_serve_one(ctx, cfd);
        if (ctx->shutdown)            /* a stop request: leave the rest queued, we are leaving */
            return;
    }
}

/* Thread entry, arg is the search_ctx. Returns on event_wake; in daemon mode also on a shutdown
 * signal or a stop request. With a TUI a signal only sets ctx->shutdown, which the TUI polls.
 */
static void *event_loop(void *arg)
{
    search_ctx *ctx = (search_ctx *)arg;
    struct epoll_event events[64];

    for (;;) {
        int nfds = epoll_wait(ctx->epoll_fd, events, 64, -1);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == ctx->shutdown_fd)
                return NULL;
            if (events[i].data.fd == ctx->signal_fd) {
                /* SIGINT/SIGTERM/SIGHUP from outside. With a TUI, ask it to leave: it exits on its
                 * next tick and main then runs the ordinary shutdown, including the save. As a
                 * daemon there is no TUI, so this loop is the one that ends.
                 */
                struct signalfd_siginfo si;
                while (read(ctx->signal_fd, &si, sizeof(si)) == sizeof(si))
                    ;
                ctx->shutdown = true;
                if (ctx->listen_fd >= 0)
                    return NULL;
                continue;
            }
            if (events[i].data.fd == ctx->ws.inotify_fd)
                scan_read_inotify(ctx);
            if (events[i].data.fd == ctx->timer_fd)
                snapshot_tick(ctx);
            if (events[i].data.fd == ctx->listen_fd) {
                accept_clients(ctx);
                if (ctx->shutdown)
                    return NULL;      /* stop over the socket ends the loop like a signal does */
            }
        }
    }
    return NULL;
}

/* The PageRank thread */

static void *pr_loop(void *arg)
{
    search_ctx *ctx = (search_ctx *)arg;

    /* A dedicated pool: the scan pool cannot be shared because tp_wait would also wait for every
     * queued scan task. Workers sleep between computations, so keeping them costs nothing.
     */
    threadpool *tp = tp_create(ctx->n_workers);
    if (!tp)
        fprintf(stderr, "pr_loop: no thread pool, computing PageRank serially\n");

    pr_node_update *dirty;
    int n_dirty;
    bool do_full;
    while (ctx_take_pr_batch(ctx, &dirty, &n_dirty, &do_full)) {
        if (n_dirty) {
            pthread_rwlock_wrlock(&ctx->graph_lock);
            pr_apply_updates(dirty, n_dirty);
            pthread_rwlock_unlock(&ctx->graph_lock);
        }

        pthread_rwlock_rdlock(&ctx->graph_lock);
        if (do_full) {
            /* Logged with the graph's size: a full computation is the longest anything waits on
             * graph_lock, and its cost turns on the edge count more than the node count (a random
             * graph with about as many edges as nodes links up into one component the residual
             * keeps circling in). Real access graphs are not random; this is how to find out where
             * they stand.
             */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            pr_compute_parallel(ctx->graph, PR_ALPHA, PR_EPSILON, tp);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long edges = 0;
            for (int i = 0; i < ctx->graph->n; i++)
                edges += pr_get(ctx->graph, i)->out_degree;
            fprintf(stderr, "pagerank: full computation, %d nodes, %ld edges, %.1f ms\n",
                    ctx->graph->n, edges,
                    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        } else {
            pr_compute_incremental(ctx->graph, dirty, n_dirty,
                                   PR_ALPHA, PR_EPSILON, tp);
        }
        pthread_rwlock_unlock(&ctx->graph_lock);

        ctx_free_pr_batch(dirty, n_dirty);
        ctx_sync_scores(ctx);
    }

    tp_destroy(tp);
    return NULL;
}

/* The background threads: they run for the whole session (event_loop waiting on epoll, pr_loop on
 * its condvar, scan_thread on the pool) and main must join them before it may save. The TUI is the
 * foreground and not one of them; main waits for it to leave on its own. Each has a flag saying
 * whether it was actually started, so that shutdown after a partial startup joins only what exists.
 */
typedef struct background {
    pthread_t event_tid, pr_tid, scan_tid;
    bool event, pr, scan;
} background;

/* Stops the background threads in dependency order: the scan first (it may still be submitting work
 * to the pool), then the event loop, and PageRank last, once nothing can produce updates.
 */
static void stop_background(search_ctx *ctx, background *bg)
{
    ctx->shutdown = true;
    scan_request_rescan(ctx);   /* wakes the scan thread so that it sees shutdown */

    if (bg->event)
        event_wake(ctx);
    if (bg->scan)
        pthread_join(bg->scan_tid, NULL);
    if (bg->event)
        pthread_join(bg->event_tid, NULL);

    tp_wait(ctx->tp);

    /* If the scan thread never ran, this is what lets pr_loop exit. */
    ctx_scan_finished(ctx, false);
    if (bg->pr)
        pthread_join(bg->pr_tid, NULL);
}

/* The save prints its own reason; a failure still leaves the previous snapshot in place. */
static int save_snapshot_and_report(search_ctx *ctx)
{
    if (snapshot_save(ctx, ctx->snapshot_path)) {
        fprintf(stderr, "Failed to save the snapshot to %s\n", ctx->snapshot_path);
        return 1;
    }
    fprintf(stderr, "Saved the snapshot to %s\n", ctx->snapshot_path);
    return 0;
}

/* --forget with no daemon answering: the snapshot is the only history there is. */
static int mode_forget(search_ctx *ctx)
{
    if (!ctx->loaded) {
        fprintf(stderr, "Nothing remembered about %s\n", ctx->root_path);
        return 0;
    }
    ctx_forget(ctx);
    if (save_snapshot_and_report(ctx))
        return 1;
    fprintf(stderr, "Forgot the access history of %s\n", ctx->root_path);
    return 0;
}

/* --query: answer from the snapshot and leave. No background threads, no inotify, no diff scan:
 * the result reflects the last save, which is what makes it fast enough for an editor or a launcher
 * to call. Without a snapshot (or with --rescan) the tree is walked first and the result saved,
 * so the next call is fast; the walk adds no inotify watches, since nothing would read them.
 *
 * Exit status: 0 with matches, 1 without, 2 on error.
 */
static int mode_query(search_ctx *ctx, const options *opt)
{
    if (!ctx->loaded || opt->rescan) {
        scan_pass(ctx, ctx->loaded);
        ctx_scan_finished(ctx, false);

        /* The snapshot has no residuals, and there is no pr_loop here to rebuild them, so a
         * loaded index is computed in place, as scan_thread asks pr_loop to. A fresh one has no
         * opens yet: nothing to compute.
         */
        if (ctx->loaded) {
            pthread_rwlock_rdlock(&ctx->graph_lock);
            pr_compute_parallel(ctx->graph, PR_ALPHA, PR_EPSILON, ctx->tp);   /* scan pool is idle now */
            pthread_rwlock_unlock(&ctx->graph_lock);
            ctx_sync_scores(ctx);
        }

        if (snapshot_save(ctx, ctx->snapshot_path))
            fprintf(stderr, "warning: could not save the snapshot to %s\n", ctx->snapshot_path);
    }

    query_reply r;
    reply_fill(ctx, opt->query, &r);
    if (r.nhits > opt->limit)
        r.nhits = opt->limit;

    if (opt->json) {
        reply_write(stdout, &r);
    } else {
        for (int i = 0; i < r.nhits; i++)
            puts(r.hits[i].path);
    }
    return r.total ? 0 : 1;
}

/* The TUI's backend when the index is in this process (--standalone): a query is a call. */
static int local_query(void *arg, const char *text, query_reply *out)
{
    reply_fill(arg, text, out);
    return 0;
}

static int local_status(void *arg, query_reply *out)
{
    reply_fill_status(arg, out);
    return 0;
}

static bool local_should_quit(void *arg)
{
    return ((search_ctx *)arg)->shutdown;
}

/* $XDG_STATE_HOME/desktop-search/<name>: where the XDG spec puts logs and state that should
 * survive a reboot. daemon.log for the daemon, standalone.log for --standalone.
 */
static bool state_log_path(const char *name, char *buf, size_t size)
{
    char dir[PATH_MAX];
    return path_xdg_dir("XDG_STATE_HOME", ".local/state", dir, sizeof(dir)) &&
           path_join(buf, size, dir, name) >= 0;
}

/* --standalone: the TUI draws on the terminal that stderr also writes to, so a background thread's
 * message (a full PageRank computation, a watch that could not be added) would land on the screen.
 * While the TUI is up, stderr goes to a log instead; restoring it says where to look if anything
 * was written.
 */
typedef struct stderr_log {
    int saved_fd;              /* the terminal's stderr while redirected, -1 otherwise */
    off_t start;               /* the log's size when the redirect began */
    char path[PATH_MAX];
} stderr_log;

static void stderr_to_log(stderr_log *lg)
{
    if (!state_log_path("standalone.log", lg->path, sizeof(lg->path)))
        return;
    int fd = open(lg->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) {
        fprintf(stderr, "%s: %s; messages stay on the terminal\n", lg->path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    fflush(stderr);
    lg->saved_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (lg->saved_fd >= 0)
        dup2(fd, STDERR_FILENO);
    lg->start = st.st_size;
    close(fd);
}

static void stderr_restore(stderr_log *lg)
{
    if (lg->saved_fd < 0)
        return;
    fflush(stderr);
    struct stat st;
    bool wrote = !fstat(STDERR_FILENO, &st) && st.st_size > lg->start;
    dup2(lg->saved_fd, STDERR_FILENO);
    close(lg->saved_fd);
    lg->saved_fd = -1;
    if (wrote)
        fprintf(stderr, "Messages from this session are in %s\n", lg->path);
}

/* Detaches from the terminal the way a classic daemon does: the parent returns to the shell at
 * once, the child lives on in its own session with no controlling terminal (so closing the terminal
 * sends it no SIGHUP), its working directory at / (so it pins no mount), stdin from /dev/null and
 * both output streams appended to the log. Must run before any thread exists: fork copies only the
 * calling thread, and a pool worker holding a lock would be gone in the child with the lock still
 * taken.
 *
 * Returns true in the child. In the parent it exits; on failure it returns false in the parent.
 */
static bool daemonize(const char *log_path)
{
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0) {
        fprintf(stderr, "%s: %s\n", log_path, strerror(errno));
        return false;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(log_fd);
        return false;
    }
    if (pid > 0) {
        fprintf(stderr, "Started (pid %d), log: %s\n", (int)pid, log_path);
        _exit(0);
    }

    if (setsid() < 0)
        perror("setsid");
    if (chdir("/"))
        perror("chdir");
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
    return true;
}

/* The modes that own an index: daemon, standalone, query and forget all create the context and load
 * the snapshot; daemon and standalone then run the background threads until told to stop.
 */
int run_with_index(const options *opt, const char *root)
{
    if (event_block_signals() == -1)
        return 2;

    /* Daemon mode: the socket is the front door instead of the TUI. It is claimed here, before
     * anything else, so that "already running" and a bad path are reported on the terminal with a
     * failing exit status; then, unless --foreground, the process detaches. Both happen before
     * ctx_create, which starts the thread pool. Only the daemon owns the socket path and may remove
     * it on the way out; --forget and --clear carry a socket path too, to reach a daemon, but never
     * claim it.
     */
    const char *own_socket = opt->mode == MODE_DAEMON ? opt->socket_path : NULL;
    int listen_fd = -1;
    if (opt->mode == MODE_DAEMON) {
        listen_fd = server_listen(opt->socket_path);
        if (listen_fd < 0)
            return 1;
        if (!opt->foreground) {
            char log_path[PATH_MAX];
            if (!state_log_path("daemon.log", log_path, sizeof(log_path)) || !daemonize(log_path)) {
                server_close(listen_fd, own_socket);
                return 1;
            }
        }
    }

    search_ctx *ctx = ctx_create(root);
    if (!ctx) {
        fprintf(stderr, "Failed to create search context\n");
        server_close(listen_fd, own_socket);
        return 2;
    }
    ctx->listen_fd = listen_fd;

    int status = 1;
    stderr_log lg = {.saved_fd = -1};
    if (snapshot_locate(ctx)) {
        fprintf(stderr, "cannot place the snapshot: no XDG_STATE_HOME or HOME\n");
        goto out;
    }

    ctx->half_life_secs = opt->half_life_days * 86400.0;
    ctx->last_decay = time(NULL);   /* a fresh index dates its weights from now; a load overrides */
    ctx->loaded = !snapshot_load(ctx, ctx->snapshot_path);

    /* The modes that only read the snapshot leave here; the rest start the background threads. */
    if (opt->mode == MODE_QUERY || opt->mode == MODE_FORGET) {
        status = opt->mode == MODE_QUERY ? mode_query(ctx, opt) : mode_forget(ctx);
        goto out;
    }

    if (ctx->loaded)
        fprintf(stderr, "Loaded the snapshot %s\n", ctx->snapshot_path);
    if (event_init(ctx, opt->snapshot_secs) == -1)
        goto out;
    if (opt->mode == MODE_DAEMON)
        fprintf(stderr, "Listening on %s\n", opt->socket_path);
    else
        stderr_to_log(&lg);   /* before the background threads start writing */

    background bg = {0};
    int err;
    if ((err = pthread_create(&bg.event_tid, NULL, event_loop, ctx))) {
        fprintf(stderr, "pthread_create(event): %s\n", strerror(err));
        goto stop;
    }
    bg.event = true;
    if ((err = pthread_create(&bg.pr_tid, NULL, pr_loop, ctx))) {
        fprintf(stderr, "pthread_create(pagerank): %s\n", strerror(err));
        goto stop;
    }
    bg.pr = true;
    if ((err = pthread_create(&bg.scan_tid, NULL, scan_thread, ctx))) {
        fprintf(stderr, "pthread_create(scan): %s\n", strerror(err));
        goto stop;
    }
    bg.scan = true;

    void *tui_result = TUI_OK;
    if (opt->mode == MODE_DAEMON) {
        /* No foreground: the event loop returns on SIGTERM/SIGINT/SIGHUP or a stop request, then
         * the rest of the background is stopped in the usual order.
         */
        pthread_join(bg.event_tid, NULL);
        bg.event = false;
    } else {
        pthread_t tui_tid;
        tui_backend be = {.arg = ctx, .query = local_query, .status = local_status,
                          .should_quit = local_should_quit};
        if ((err = pthread_create(&tui_tid, NULL, tui_loop, &be))) {
            fprintf(stderr, "pthread_create(tui): %s\n", strerror(err));
            goto stop;
        }
        pthread_join(tui_tid, &tui_result);
    }
    stop_background(ctx, &bg);
    bg = (background){0};
    stderr_restore(&lg);   /* the save's own message is for the terminal */

    status = save_snapshot_and_report(ctx);
    if (tui_result != TUI_OK)
        status = 1;
    goto out;

stop:
    stop_background(ctx, &bg);
out:
    stderr_restore(&lg);
    server_close(ctx->listen_fd, own_socket);
    event_close(ctx);
    ctx_destroy(ctx);
    return status;
}
