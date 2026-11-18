#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/time.h>

#define MAX_REQ_SIZE (64 * 1024)
#define MAX_RESP_SIZE (64 * 1024)

typedef struct route_node {
    char method[16];
    char path[256];
    route_handler_cb handler;
    struct route_node* next;
} route_node_t;

typedef struct {
    int client_fd;
} client_worker_arg_t;

static route_node_t* s_routes = NULL;
static pthread_mutex_t s_routes_mutex = PTHREAD_MUTEX_INITIALIZER;

static int s_listen_fd = -1;
static int s_server_running = 0;
static pthread_t s_listener_thread;
static pthread_mutex_t s_server_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_workers_cond = PTHREAD_COND_INITIALIZER;
static int s_active_workers = 0;

static const char* find_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return haystack;
    for (const char* p = haystack; *p != '\0'; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return NULL;
}

static int send_all(int fd, const char* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t n = send(fd, data + total_sent, len - total_sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total_sent += (size_t)n;
    }
    return 0;
}

int server_register_route(const char* method, const char* path, void (*handler)(const char* req_body, char* resp_out, size_t max_resp)) {
    if (!method || !path || !handler) {
        return -1;
    }

    pthread_mutex_lock(&s_routes_mutex);

    // Check if route already exists; if so, update handler
    route_node_t* cur = s_routes;
    while (cur) {
        if (strcasecmp(cur->method, method) == 0 && strcmp(cur->path, path) == 0) {
            cur->handler = handler;
            pthread_mutex_unlock(&s_routes_mutex);
            return 0;
        }
        cur = cur->next;
    }

    route_node_t* node = calloc(1, sizeof(route_node_t));
    if (!node) {
        pthread_mutex_unlock(&s_routes_mutex);
        return -1;
    }

    snprintf(node->method, sizeof(node->method), "%s", method);
    snprintf(node->path, sizeof(node->path), "%s", path);
    node->handler = handler;
    node->next = s_routes;
    s_routes = node;

    pthread_mutex_unlock(&s_routes_mutex);
    return 0;
}

void server_clear_routes(void) {
    pthread_mutex_lock(&s_routes_mutex);
    route_node_t* cur = s_routes;
    while (cur) {
        route_node_t* next = cur->next;
        free(cur);
        cur = next;
    }
    s_routes = NULL;
    pthread_mutex_unlock(&s_routes_mutex);
}

static void send_cors_preflight_response(int fd) {
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS, PUT, DELETE\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    send_all(fd, resp, strlen(resp));
}

static void send_not_found_response(int fd) {
    const char* not_found_body = "{\"error\":\"Not Found\"}";
    size_t body_len = strlen(not_found_body);
    char header_buf[512];
    int hlen = snprintf(header_buf, sizeof(header_buf),
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);
    send_all(fd, header_buf, (size_t)hlen);
    send_all(fd, not_found_body, body_len);
}

static void send_bad_request_response(int fd) {
    const char* body = "{\"error\":\"Bad Request\"}";
    size_t body_len = strlen(body);
    char header_buf[512];
    int hlen = snprintf(header_buf, sizeof(header_buf),
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);
    send_all(fd, header_buf, (size_t)hlen);
    send_all(fd, body, body_len);
}

static void* connection_worker(void* arg) {
    client_worker_arg_t* warg = (client_worker_arg_t*)arg;
    int client_fd = warg->client_fd;
    free(warg);

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char* req_buf = malloc(MAX_REQ_SIZE);
    if (!req_buf) {
        close(client_fd);
        pthread_mutex_lock(&s_server_mutex);
        s_active_workers--;
        if (s_active_workers == 0) {
            pthread_cond_broadcast(&s_workers_cond);
        }
        pthread_mutex_unlock(&s_server_mutex);
        return NULL;
    }

    size_t total_recv = 0;
    int header_end_found = 0;
    char* header_end = NULL;

    while (total_recv < MAX_REQ_SIZE - 1) {
        ssize_t n = recv(client_fd, req_buf + total_recv, MAX_REQ_SIZE - 1 - total_recv, 0);
        if (n <= 0) {
            break;
        }
        total_recv += (size_t)n;
        req_buf[total_recv] = '\0';

        header_end = strstr(req_buf, "\r\n\r\n");
        if (header_end) {
            header_end_found = 1;
            break;
        }
        header_end = strstr(req_buf, "\n\n");
        if (header_end) {
            header_end_found = 1;
            break;
        }
    }

    if (!header_end_found || !header_end) {
        free(req_buf);
        close(client_fd);
        pthread_mutex_lock(&s_server_mutex);
        s_active_workers--;
        if (s_active_workers == 0) {
            pthread_cond_broadcast(&s_workers_cond);
        }
        pthread_mutex_unlock(&s_server_mutex);
        return NULL;
    }

    size_t sep_len = (strstr(req_buf, "\r\n\r\n") != NULL) ? 4 : 2;
    size_t header_len = (size_t)(header_end - req_buf) + sep_len;
    char* body_start = req_buf + header_len;

    // Check Content-Length
    size_t content_len = 0;
    const char* cl_ptr = find_case_insensitive(req_buf, "Content-Length:");
    if (cl_ptr && cl_ptr < header_end) {
        content_len = (size_t)strtoul(cl_ptr + 15, NULL, 10);
    }

    size_t body_recv = total_recv - header_len;
    while (body_recv < content_len && total_recv < MAX_REQ_SIZE - 1) {
        ssize_t n = recv(client_fd, req_buf + total_recv, MAX_REQ_SIZE - 1 - total_recv, 0);
        if (n <= 0) {
            break;
        }
        total_recv += (size_t)n;
        body_recv += (size_t)n;
        req_buf[total_recv] = '\0';
    }

    body_start[content_len] = '\0';

    // Parse method and path
    char method[16] = {0};
    char raw_path[512] = {0};
    if (sscanf(req_buf, "%15s %511s", method, raw_path) < 2) {
        send_bad_request_response(client_fd);
        free(req_buf);
        close(client_fd);
        pthread_mutex_lock(&s_server_mutex);
        s_active_workers--;
        if (s_active_workers == 0) {
            pthread_cond_broadcast(&s_workers_cond);
        }
        pthread_mutex_unlock(&s_server_mutex);
        return NULL;
    }

    // Handle OPTIONS CORS preflight generically
    if (strcasecmp(method, "OPTIONS") == 0) {
        send_cors_preflight_response(client_fd);
        free(req_buf);
        close(client_fd);
        pthread_mutex_lock(&s_server_mutex);
        s_active_workers--;
        if (s_active_workers == 0) {
            pthread_cond_broadcast(&s_workers_cond);
        }
        pthread_mutex_unlock(&s_server_mutex);
        return NULL;
    }

    // Normalize path by stripping query parameters and trailing slash
    char path[256];
    snprintf(path, sizeof(path), "%s", raw_path);
    char* q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }
    size_t plen = strlen(path);
    if (plen > 1 && path[plen - 1] == '/') {
        path[plen - 1] = '\0';
    }

    // Match registered route
    route_handler_cb handler = NULL;
    pthread_mutex_lock(&s_routes_mutex);
    route_node_t* cur = s_routes;
    while (cur) {
        if (strcasecmp(cur->method, method) == 0 && strcmp(cur->path, path) == 0) {
            handler = cur->handler;
            break;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&s_routes_mutex);

    if (handler) {
        char* resp_out = calloc(1, MAX_RESP_SIZE);
        if (resp_out) {
            handler(body_start, resp_out, MAX_RESP_SIZE);

            if (strncmp(resp_out, "HTTP/1.", 7) == 0) {
                // Raw HTTP response formatted by handler
                send_all(client_fd, resp_out, strlen(resp_out));
            } else {
                size_t resp_len = strlen(resp_out);
                char header_buf[512];
                int hlen = snprintf(header_buf, sizeof(header_buf),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json; charset=utf-8\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: *\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    resp_len);
                send_all(client_fd, header_buf, (size_t)hlen);
                if (resp_len > 0) {
                    send_all(client_fd, resp_out, resp_len);
                }
            }
            free(resp_out);
        } else {
            send_bad_request_response(client_fd);
        }
    } else {
        send_not_found_response(client_fd);
    }

    free(req_buf);
    close(client_fd);

    pthread_mutex_lock(&s_server_mutex);
    s_active_workers--;
    if (s_active_workers == 0) {
        pthread_cond_broadcast(&s_workers_cond);
    }
    pthread_mutex_unlock(&s_server_mutex);

    return NULL;
}

static void* listener_thread_main(void* arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&s_server_mutex);
        if (!s_server_running || s_listen_fd < 0) {
            pthread_mutex_unlock(&s_server_mutex);
            break;
        }
        int lfd = s_listen_fd;
        pthread_mutex_unlock(&s_server_mutex);

        struct pollfd pfd;
        pfd.fd = lfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 100); // 100ms timeout
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ret == 0) {
            continue;
        }

        if (pfd.revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(lfd, (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd < 0) {
                continue;
            }

            pthread_mutex_lock(&s_server_mutex);
            if (!s_server_running) {
                pthread_mutex_unlock(&s_server_mutex);
                close(client_fd);
                break;
            }
            s_active_workers++;
            pthread_mutex_unlock(&s_server_mutex);

            client_worker_arg_t* warg = malloc(sizeof(client_worker_arg_t));
            if (!warg) {
                close(client_fd);
                pthread_mutex_lock(&s_server_mutex);
                s_active_workers--;
                if (s_active_workers == 0) {
                    pthread_cond_broadcast(&s_workers_cond);
                }
                pthread_mutex_unlock(&s_server_mutex);
                continue;
            }

            warg->client_fd = client_fd;

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            int rc = pthread_create(&tid, &attr, connection_worker, warg);
            pthread_attr_destroy(&attr);

            if (rc != 0) {
                free(warg);
                close(client_fd);
                pthread_mutex_lock(&s_server_mutex);
                s_active_workers--;
                if (s_active_workers == 0) {
                    pthread_cond_broadcast(&s_workers_cond);
                }
                pthread_mutex_unlock(&s_server_mutex);
            }
        }
    }

    return NULL;
}

int server_start(int port) {
    if (port <= 0 || port > 65535) {
        return -1;
    }

    pthread_mutex_lock(&s_server_mutex);
    if (s_server_running) {
        pthread_mutex_unlock(&s_server_mutex);
        return 0; // Already started
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        pthread_mutex_unlock(&s_server_mutex);
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        pthread_mutex_unlock(&s_server_mutex);
        return -1;
    }

    if (listen(fd, 64) != 0) {
        close(fd);
        pthread_mutex_unlock(&s_server_mutex);
        return -1;
    }

    s_listen_fd = fd;
    s_server_running = 1;

    int rc = pthread_create(&s_listener_thread, NULL, listener_thread_main, NULL);
    if (rc != 0) {
        s_server_running = 0;
        s_listen_fd = -1;
        close(fd);
        pthread_mutex_unlock(&s_server_mutex);
        return -1;
    }

    pthread_mutex_unlock(&s_server_mutex);
    return 0;
}

void server_stop(void) {
    pthread_mutex_lock(&s_server_mutex);
    if (!s_server_running && s_listen_fd < 0) {
        pthread_mutex_unlock(&s_server_mutex);
        return;
    }

    s_server_running = 0;
    int fd_to_close = s_listen_fd;
    s_listen_fd = -1;
    pthread_mutex_unlock(&s_server_mutex);

    if (fd_to_close >= 0) {
        shutdown(fd_to_close, SHUT_RDWR);
        close(fd_to_close);
    }

    if (!pthread_equal(pthread_self(), s_listener_thread)) {
        pthread_join(s_listener_thread, NULL);
    } else {
        pthread_detach(s_listener_thread);
    }

    // Wait for all active connection workers to finish
    pthread_mutex_lock(&s_server_mutex);
    while (s_active_workers > 0) {
        pthread_cond_wait(&s_workers_cond, &s_server_mutex);
    }
    pthread_mutex_unlock(&s_server_mutex);
}
