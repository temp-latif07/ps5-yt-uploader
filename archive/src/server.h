#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*route_handler_cb)(const char* req_body, char* resp_out, size_t max_resp);

/**
 * Start the HTTP server listening on the specified port.
 * Runs in a background thread and returns immediately.
 *
 * @param port Port number to bind and listen on
 * @return 0 on success, -1 on failure
 */
int server_start(int port);

/**
 * Stop the HTTP server, close the listening socket, and terminate threads.
 */
void server_stop(void);

/**
 * Register a route handler for a given HTTP method and path.
 *
 * @param method HTTP method ("GET", "POST", etc.)
 * @param path URL path (e.g. "/api/status")
 * @param handler Callback to invoke when route matches
 * @return 0 on success, -1 on failure
 */
int server_register_route(const char* method, const char* path, void (*handler)(const char* req_body, char* resp_out, size_t max_resp));

/**
 * Clear all registered routes.
 */
void server_clear_routes(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
