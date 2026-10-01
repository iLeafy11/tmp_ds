#ifndef _RUN_H
#define _RUN_H

#include "ctx.h"

typedef enum run_mode {
    MODE_ATTACH,       /* DIR: find or start a daemon for DIR, then the TUI on it */
    MODE_STANDALONE,   /* standalone DIR: index and TUI in this process, save on exit */
    MODE_QUERY,        /* query TEXT DIR: answer from the snapshot and leave */
    MODE_DAEMON,       /* daemon DIR: index behind the socket, no TUI */
    MODE_STOP,         /* stop: ask the running daemon to save and exit */
    MODE_FORGET,       /* forget DIR: drop DIR's access history, keep its index */
    MODE_CLEAR,        /* clear: remove every snapshot of every root */
} run_mode;

typedef struct options {
    run_mode mode;
    const char *root;
    int root_argi;             /* argv index of root, so it can be rewritten as an absolute path */
    const char *query;
    bool json;
    bool rescan;
    int limit;
    int snapshot_secs;         /* seconds between periodic snapshots; 0 = only at exit */
    double half_life_days;     /* edge-weight decay; 0 = never decay */
    bool snapshot_given;       /* --snapshot was on the command line, even with the default value */
    bool half_life_given;      /* --half-life likewise */
    bool foreground;           /* the daemon stays attached to the terminal (systemd, tests) */
    char socket_path[PATH_MAX];
} options;

/* The modes that own an index (daemon, standalone, query, forget): creates the context, loads the
 * snapshot, runs the mode, saves, and cleans up. Returns the process exit status.
 */
int run_with_index(const options *opt, const char *root);

#endif /* _RUN_H */
