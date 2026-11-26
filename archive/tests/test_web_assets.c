#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define TEST_PORT 8890
#define MAX_BODY_BUF (256 * 1024)
#define MAX_HDR_BUF 4096

static const char* ci_strstr(const char* haystack, const char* needle) {
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

static int http_get(int port, const char* path, int* out_status,
                    char* out_headers, size_t max_headers,
                    char* out_body, size_t max_body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "User-Agent: test-client/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, port);

    if (send(sock, req, req_len, 0) != req_len) {
        close(sock);
        return -1;
    }

    size_t total_recv = 0;
    size_t resp_buf_size = 512 * 1024;
    char* full_resp = malloc(resp_buf_size);
    if (!full_resp) {
        close(sock);
        return -1;
    }

    while (total_recv < resp_buf_size - 1) {
        ssize_t n = recv(sock, full_resp + total_recv, resp_buf_size - 1 - total_recv, 0);
        if (n <= 0) break;
        total_recv += (size_t)n;
    }
    full_resp[total_recv] = '\0';
    close(sock);

    char* sep = strstr(full_resp, "\r\n\r\n");
    size_t sep_len = 4;
    if (!sep) {
        sep = strstr(full_resp, "\n\n");
        sep_len = 2;
    }
    if (!sep) {
        free(full_resp);
        return -1;
    }

    size_t header_len = (size_t)(sep - full_resp);
    char* body_start = sep + sep_len;
    size_t body_len = total_recv - (header_len + sep_len);

    if (out_status) {
        int code = 0;
        if (sscanf(full_resp, "HTTP/1.%*d %d", &code) == 1) {
            *out_status = code;
        } else {
            *out_status = 0;
        }
    }

    if (out_headers && max_headers > 0) {
        size_t copy_len = (header_len < max_headers - 1) ? header_len : (max_headers - 1);
        memcpy(out_headers, full_resp, copy_len);
        out_headers[copy_len] = '\0';
    }

    if (out_body && max_body > 0) {
        size_t copy_len = (body_len < max_body - 1) ? body_len : (max_body - 1);
        memcpy(out_body, body_start, copy_len);
        out_body[copy_len] = '\0';
    }

    free(full_resp);
    return 0;
}

int main(void) {
    printf("Starting embedded HTTP server on port %d for web asset testing...\n", TEST_PORT);
    int rc = server_start(TEST_PORT);
    assert(rc == 0);
    usleep(100000); // 100ms for listener thread startup

    char headers[MAX_HDR_BUF];
    char* body = malloc(MAX_BODY_BUF);
    assert(body != NULL);
    int status = 0;

    // Test 1: GET / (Root URL delivers HTML dashboard)
    printf("[Test 1] Testing GET / (embedded HTML dashboard)...\n");
    memset(headers, 0, sizeof(headers));
    memset(body, 0, MAX_BODY_BUF);
    rc = http_get(TEST_PORT, "/", &status, headers, sizeof(headers), body, MAX_BODY_BUF);
    assert(rc == 0);
    assert(status == 200);
    assert(ci_strstr(headers, "Content-Type: text/html") != NULL);
    assert(ci_strstr(headers, "Cache-Control: no-cache") != NULL);
    assert(strstr(body, "<title>PS5 YouTube Uploader</title>") != NULL);
    assert(strstr(body, "Clips Gallery") != NULL);
    assert(strstr(body, "modal-select-kids") != NULL);
    assert(strstr(body, "modal-input-playlist") != NULL);
    printf("  -> GET / returned 200 OK with Content-Type: text/html and title confirmed!\n");

    // Test 2: GET /index.html (Alias path delivers HTML dashboard)
    printf("[Test 2] Testing GET /index.html...\n");
    memset(headers, 0, sizeof(headers));
    memset(body, 0, MAX_BODY_BUF);
    rc = http_get(TEST_PORT, "/index.html", &status, headers, sizeof(headers), body, MAX_BODY_BUF);
    assert(rc == 0);
    assert(status == 200);
    assert(ci_strstr(headers, "Content-Type: text/html") != NULL);
    assert(strstr(body, "<title>PS5 YouTube Uploader</title>") != NULL);
    printf("  -> GET /index.html returned 200 OK with Content-Type: text/html confirmed!\n");

    // Test 3: GET /style.css (Embedded CSS stylesheet)
    printf("[Test 3] Testing GET /style.css (embedded CSS styling)...\n");
    memset(headers, 0, sizeof(headers));
    memset(body, 0, MAX_BODY_BUF);
    rc = http_get(TEST_PORT, "/style.css", &status, headers, sizeof(headers), body, MAX_BODY_BUF);
    assert(rc == 0);
    assert(status == 200);
    assert(ci_strstr(headers, "Content-Type: text/css") != NULL);
    assert(ci_strstr(headers, "Cache-Control: no-cache") != NULL);
    assert(strstr(body, "ps5-theme") != NULL);
    printf("  -> GET /style.css returned 200 OK with Content-Type: text/css confirmed!\n");

    // Test 4: GET /app.js (Embedded vanilla JS script)
    printf("[Test 4] Testing GET /app.js (embedded vanilla JS application)...\n");
    memset(headers, 0, sizeof(headers));
    memset(body, 0, MAX_BODY_BUF);
    rc = http_get(TEST_PORT, "/app.js", &status, headers, sizeof(headers), body, MAX_BODY_BUF);
    assert(rc == 0);
    assert(status == 200);
    assert(ci_strstr(headers, "Content-Type: application/javascript") != NULL);
    assert(ci_strstr(headers, "Cache-Control: no-cache") != NULL);
    assert(strstr(body, "renderQRCodeSVG") != NULL);
    printf("  -> GET /app.js returned 200 OK with Content-Type: application/javascript confirmed!\n");

    // Test 5: Verify clean server shutdown
    printf("[Test 5] Stopping HTTP server...\n");
    server_stop();
    usleep(100000);

    // Verify port closed
    int test_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    rc = connect(test_sock, (struct sockaddr*)&addr, sizeof(addr));
    assert(rc != 0); // Must fail to connect
    close(test_sock);
    printf("  -> Verified server port %d closed cleanly!\n", TEST_PORT);

    free(body);
    printf("Task 6 Web Asset Delivery Tests Passed Successfully!\n");
    return 0;
}
