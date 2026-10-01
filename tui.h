#ifndef _TUI_H
#define _TUI_H

#include "reply.h"

/* Where the TUI gets its answers. The same loop runs against the index in this process (a query is
 * a function call) or against a daemon over the socket (a query is a round trip); the TUI itself
 * holds no index either way, only the last reply.
 */
typedef struct tui_backend {
    void *arg;

    /* Runs text and fills out. -1 means the backend is gone (daemon not running); the TUI keeps
     * showing the last reply and says so in the label.
     */
    int (*query)(void *arg, const char *text, query_reply *out);
    /* Fills only out->status (and root); what the idle tick polls instead of rerunning the query. */
    int (*status)(void *arg, query_reply *out);
    /* True once the process is shutting down (a signal arrived); NULL if nothing can ask. */
    bool (*should_quit)(void *arg);
} tui_backend;

/* Thread entry; arg is a tui_backend *.
 *
 * Returns TUI_OK when the terminal was set up and the loop ran; anything else means the UI never
 * came up (tb_init or allocation failed) and main should exit with an error.
 */
#define TUI_OK ((void *)1)
void *tui_loop(void *arg);

#endif
