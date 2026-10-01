#ifndef CLIENT_H
#define CLIENT_H

#include "reply.h"

/* The client side of the socket protocol: connect, send one query line, read the reply line back
 * into a query_reply.
 *
 * Returns 0, or -1 when the daemon is not running, does not answer in time, or answers something
 * that is not a reply; client_error then says which. Nothing in this module prints.
 */
int client_query(const char *sock_path, const char *text, query_reply *out);

/* Asks for the status only (the reply has no hits). Same return values as client_query. */
int client_status(const char *sock_path, query_reply *out);

/* Asks the daemon to forget its access history (the index stays). Returns 0 once it acknowledged,
 * -1 if it is not running or did not answer.
 */
int client_clear(const char *sock_path);

/* Asks the daemon to save and exit. Returns 0 once it acknowledged, -1 if it is not running or did
 * not answer.
 */
int client_stop(const char *sock_path);

/* The reason the last call returned -1, one line with no newline. */
const char *client_error(void);


#endif /* CLIENT_H */
