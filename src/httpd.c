// Minimal HTTP/1.1 server: short-lived thread per connection, "Connection: close".
// Every socket wait goes through poll(): PS5 ignores O_NONBLOCK/FIONBIO on sockets.
#include "app.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CONNS 24
#define MAX_HEADER 16384
#define MAX_BODY (256 * 1024)

static volatile int g_stop;
static int g_conns;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int wait_fd(int fd, short ev, int ms) {
    struct pollfd p = {fd, ev, 0};
    int r;
    do r = poll(&p, 1, ms);
    while (r < 0 && errno == EINTR);
    return r;
}

int http_send_raw(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        if (wait_fd(fd, POLLOUT, 30000) <= 0) return -1;
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static const char *status_text(int s) {
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Status";
    }
}

void http_send(int fd, int status, const char *ctype, const void *body, size_t len, const char *extra) {
    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n",
                     status, status_text(status), ctype, len, extra ? extra : "");
    if (n > 0 && http_send_raw(fd, head, (size_t)n) == 0 && len)
        http_send_raw(fd, body, len);
}

void http_send_json(int fd, int status, const sbuf *b) {
    http_send(fd, status, "application/json; charset=utf-8",
              b && b->data ? b->data : "{}", b && b->data ? b->len : 2, NULL);
}

static void handle(int fd) {
    char *head = malloc(MAX_HEADER + 1);
    http_req req;
    memset(&req, 0, sizeof req);
    req.fd = fd;
    size_t got = 0;
    char *end = NULL;
    while (head && got < MAX_HEADER) {
        if (wait_fd(fd, POLLIN, 15000) <= 0) goto out;
        ssize_t n = recv(fd, head + got, MAX_HEADER - got, 0);
        if (n <= 0) goto out;
        got += (size_t)n;
        head[got] = 0;
        if ((end = strstr(head, "\r\n\r\n"))) break;
    }
    if (!end) {
        if (head) http_send(fd, 400, "text/plain", "bad request", 11, NULL);
        goto out;
    }
    *end = 0;
    char *body_start = end + 4;
    size_t body_have = got - (size_t)(body_start - head);

    char target[sizeof req.path + sizeof req.query];
    if (sscanf(head, "%7s %5119s", req.method, target) != 2) {
        http_send(fd, 400, "text/plain", "bad request", 11, NULL);
        goto out;
    }
    char *q = strchr(target, '?');
    if (q) {
        *q = 0;
        snprintf(req.query, sizeof req.query, "%s", q + 1);
    }
    snprintf(req.path, sizeof req.path, "%s", target);

    size_t clen = 0;
    for (char *line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        char *h = line + 2;
        if (!strncasecmp(h, "Content-Length:", 15)) {
            clen = strtoul(h + 15, NULL, 10);
        } else if (!strncasecmp(h, "X-PS5YT:", 8)) {
            req.has_app_header = 1;
        } else if (!strncasecmp(h, "Range:", 6)) {
            char *v = h + 6;
            while (*v == ' ' || *v == '\t') v++;
            char *eol = strstr(v, "\r\n");
            size_t rlen = eol ? (size_t)(eol - v) : strlen(v);
            if (rlen >= sizeof req.range) rlen = sizeof req.range - 1;
            memcpy(req.range, v, rlen);
            req.range[rlen] = 0;
        }
    }

    if (clen > MAX_BODY) {
        http_send(fd, 413, "text/plain", "too large", 9, NULL);
        goto out;
    }
    req.body = calloc(1, clen + 1);
    if (!req.body) goto out;
    size_t blen = body_have < clen ? body_have : clen;
    if (blen > 0) memcpy(req.body, body_start, blen);
    while (blen < clen) {
        if (wait_fd(fd, POLLIN, 15000) <= 0) goto out;
        ssize_t n = recv(fd, req.body + blen, clen - blen, 0);
        if (n <= 0) goto out;
        blen += (size_t)n;
    }
    req.body[blen] = 0;
    req.body_len = blen;
    api_handle(&req);
out:
    if (req.body) {
        memset(req.body, 0, req.body_len);
        free(req.body);
    }
    if (head) {
        memset(head, 0, MAX_HEADER + 1);
        free(head);
    }
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle(fd);
    shutdown(fd, SHUT_WR);
    char tmp[512];
    for (int i = 0; i < 4 && wait_fd(fd, POLLIN, 200) > 0 && recv(fd, tmp, sizeof tmp, 0) > 0; i++) {}
    close(fd);
    pthread_mutex_lock(&g_conn_lock);
    g_conns--;
    pthread_mutex_unlock(&g_conn_lock);
    return NULL;
}

int httpd_run(int port) {
    g_stop = 0;
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 32)) {
        plat_log("cannot listen on port %d: %s", port, strerror(errno));
        close(s);
        return -1;
    }
    plat_log("listening on port %d", port);
    while (!g_stop) {
        if (wait_fd(s, POLLIN, 500) <= 0) continue;
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        pthread_mutex_lock(&g_conn_lock);
        int busy = g_conns >= MAX_CONNS;
        if (!busy) g_conns++;
        pthread_mutex_unlock(&g_conn_lock);
        if (busy || plat_start_thread(conn_thread, (void *)(intptr_t)c)) {
            if (!busy) {
                pthread_mutex_lock(&g_conn_lock);
                g_conns--;
                pthread_mutex_unlock(&g_conn_lock);
            }
            http_send(c, 500, "text/plain", "busy", 4, NULL);
            close(c);
        }
    }
    close(s);
    for (int i = 0; i < 50 && g_conns; i++) plat_sleep_ms(100);
    return 0;
}

void httpd_stop(void) { g_stop = 1; }
