#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0
#endif

void http_response_init(http_response_t* resp) {
    if (!resp) return;
    memset(resp, 0, sizeof(*resp));
}

void http_response_cleanup(http_response_t* resp) {
    if (!resp) return;

    if (resp->headers) {
        for (size_t i = 0; i < resp->header_count; i++) {
            free(resp->headers[i].value);
        }
        free(resp->headers);
        resp->headers = NULL;
    }
    resp->header_count = 0;
    resp->header_capacity = 0;

    if (resp->body) {
        free(resp->body);
        resp->body = NULL;
    }
    resp->body_len = 0;
    resp->status_code = 0;
    resp->status_message[0] = '\0';
}

const char* http_response_get_header(const http_response_t* resp, const char* name) {
    if (!resp || !name || !resp->headers) return NULL;

    for (size_t i = 0; i < resp->header_count; i++) {
        if (strcasecmp(resp->headers[i].name, name) == 0) {
            return resp->headers[i].value;
        }
    }
    return NULL;
}

static int add_response_header(http_response_t* resp, const char* name, const char* value) {
    if (resp->header_count >= resp->header_capacity) {
        size_t new_cap = resp->header_capacity ? resp->header_capacity * 2 : 16;
        http_header_t* new_headers = (http_header_t*)realloc(resp->headers, new_cap * sizeof(http_header_t));
        if (!new_headers) return -1;
        resp->headers = new_headers;
        resp->header_capacity = new_cap;
    }

    http_header_t* h = &resp->headers[resp->header_count];
    strncpy(h->name, name, sizeof(h->name) - 1);
    h->name[sizeof(h->name) - 1] = '\0';

    h->value = strdup(value ? value : "");
    if (!h->value) return -1;

    resp->header_count++;
    return 0;
}

static char* trim_whitespace(char* str) {
    if (!str) return NULL;
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;

    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

static int parse_url(const char* url, char* host_out, size_t host_max, int* port_out, char* path_out, size_t path_max) {
    if (!url || !host_out || !port_out || !path_out) return -1;

    const char* p = url;
    int is_https = 0;

    if (strncasecmp(p, "http://", 7) == 0) {
        p += 7;
        *port_out = 80;
    } else if (strncasecmp(p, "https://", 8) == 0) {
        p += 8;
        *port_out = 443;
        is_https = 1;
    } else {
        return -1; // Unsupported or missing scheme
    }
    (void)is_https;

    // Parse host and optional port
    const char* host_start = p;
    while (*p && *p != ':' && *p != '/') {
        p++;
    }

    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= host_max) return -1;
    memcpy(host_out, host_start, host_len);
    host_out[host_len] = '\0';

    if (*p == ':') {
        p++;
        char* endp = NULL;
        long port = strtol(p, &endp, 10);
        if (port <= 0 || port > 65535) return -1;
        *port_out = (int)port;
        p = endp;
    }

    if (*p == '\0') {
        strncpy(path_out, "/", path_max - 1);
        path_out[path_max - 1] = '\0';
    } else {
        strncpy(path_out, p, path_max - 1);
        path_out[path_max - 1] = '\0';
    }

    return 0;
}

static ssize_t send_all(int sock, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

int http_client_request(const http_request_t* req, http_response_t* resp) {
    if (!req || !resp || !req->url || !req->method) return -1;

    // Ignore SIGPIPE to avoid crashing when server abruptly closes connection mid-stream
    signal(SIGPIPE, SIG_IGN);

    char host[256];
    char path[2048];
    int port = 80;

    if (parse_url(req->url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        return -1;
    }

    // Connect to host:port
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int sock = -1;
    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;

        // Set timeout
        int timeout_sec = (req->timeout_sec > 0) ? req->timeout_sec : 10;
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

#if defined(SO_NOSIGPIPE) && SO_NOSIGPIPE != 0
        int nosigpipe = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            break; // Successfully connected
        }

        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        return -1;
    }

    // Determine body length
    size_t body_content_length = 0;
    size_t total_stream_file_size = 0;
    FILE* file_fp = NULL;

    if (req->body_filepath) {
        struct stat st;
        if (stat(req->body_filepath, &st) != 0) {
            close(sock);
            return -1;
        }
        total_stream_file_size = (size_t)st.st_size;

        if (req->file_length > 0) {
            body_content_length = req->file_length;
        } else if (total_stream_file_size >= req->file_offset) {
            body_content_length = total_stream_file_size - req->file_offset;
        } else {
            close(sock);
            return -1;
        }

        file_fp = fopen(req->body_filepath, "rb");
        if (!file_fp) {
            close(sock);
            return -1;
        }

        if (req->file_offset > 0) {
            if (fseek(file_fp, (long)req->file_offset, SEEK_SET) != 0) {
                fclose(file_fp);
                close(sock);
                return -1;
            }
        }
    } else if (req->body && req->body_len > 0) {
        body_content_length = req->body_len;
    }

    // Check if Content-Length is explicitly set in req->headers
    int has_content_length = 0;
    if (req->headers) {
        for (size_t i = 0; i < req->header_count; i++) {
            if (strcasecmp(req->headers[i].name, "Content-Length") == 0) {
                has_content_length = 1;
                break;
            }
        }
    }

    // Build headers buffer
    size_t req_buf_cap = 16384;
    char* req_buf = (char*)malloc(req_buf_cap);
    if (!req_buf) {
        if (file_fp) fclose(file_fp);
        close(sock);
        return -1;
    }

    int written = snprintf(req_buf, req_buf_cap,
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "User-Agent: ps5-youtube-uploader/1.0\r\n"
                           "Connection: close\r\n",
                           req->method, path, host, port);

    if (written < 0 || (size_t)written >= req_buf_cap) {
        free(req_buf);
        if (file_fp) fclose(file_fp);
        close(sock);
        return -1;
    }
    size_t req_buf_len = (size_t)written;

    // Append custom headers
    if (req->headers) {
        for (size_t i = 0; i < req->header_count; i++) {
            size_t needed = strlen(req->headers[i].name) + (req->headers[i].value ? strlen(req->headers[i].value) : 0) + 16;
            if (req_buf_len + needed >= req_buf_cap) {
                req_buf_cap = (req_buf_len + needed) * 2;
                char* expanded = (char*)realloc(req_buf, req_buf_cap);
                if (!expanded) {
                    free(req_buf);
                    if (file_fp) fclose(file_fp);
                    close(sock);
                    return -1;
                }
                req_buf = expanded;
            }
            int h_written = snprintf(req_buf + req_buf_len, req_buf_cap - req_buf_len,
                                     "%s: %s\r\n",
                                     req->headers[i].name,
                                     req->headers[i].value ? req->headers[i].value : "");
            if (h_written > 0) {
                req_buf_len += (size_t)h_written;
            }
        }
    }

    // Add Content-Length if not explicitly provided
    if (!has_content_length) {
        char cl_buf[64];
        int cl_len = snprintf(cl_buf, sizeof(cl_buf), "Content-Length: %zu\r\n", body_content_length);
        if (req_buf_len + (size_t)cl_len + 4 >= req_buf_cap) {
            req_buf_cap = (req_buf_len + (size_t)cl_len + 64);
            char* expanded = (char*)realloc(req_buf, req_buf_cap);
            if (!expanded) {
                free(req_buf);
                if (file_fp) fclose(file_fp);
                close(sock);
                return -1;
            }
            req_buf = expanded;
        }
        memcpy(req_buf + req_buf_len, cl_buf, (size_t)cl_len);
        req_buf_len += (size_t)cl_len;
    }

    // Add final CRLF
    memcpy(req_buf + req_buf_len, "\r\n", 2);
    req_buf_len += 2;

    // Send HTTP headers
    if (send_all(sock, req_buf, req_buf_len) < 0) {
        free(req_buf);
        if (file_fp) fclose(file_fp);
        close(sock);
        return -1;
    }
    free(req_buf);
    req_buf = NULL;

    // Send HTTP body
    if (file_fp) {
        char chunk[65536];
        size_t sent_bytes = 0;
        while (sent_bytes < body_content_length) {
            size_t to_read = (body_content_length - sent_bytes > sizeof(chunk)) ? sizeof(chunk) : (body_content_length - sent_bytes);
            size_t read_bytes = fread(chunk, 1, to_read, file_fp);
            if (read_bytes == 0) break;

            if (send_all(sock, chunk, read_bytes) < 0) {
                fclose(file_fp);
                close(sock);
                return -1;
            }
            sent_bytes += read_bytes;

            if (req->progress_cb) {
                req->progress_cb(req->file_offset + sent_bytes, total_stream_file_size, req->userdata);
            }
        }
        fclose(file_fp);
        file_fp = NULL;

        if (sent_bytes < body_content_length) {
            close(sock);
            return -1;
        }
    } else if (req->body && req->body_len > 0) {
        if (send_all(sock, req->body, req->body_len) < 0) {
            close(sock);
            return -1;
        }
        if (req->progress_cb) {
            req->progress_cb(req->body_len, req->body_len, req->userdata);
        }
    }

    // Read response headers
    size_t raw_cap = 8192;
    char* raw_buf = (char*)malloc(raw_cap);
    if (!raw_buf) {
        close(sock);
        return -1;
    }
    size_t raw_len = 0;
    char* header_end = NULL;

    while (!header_end) {
        if (raw_len + 2048 >= raw_cap) {
            raw_cap *= 2;
            char* exp = (char*)realloc(raw_buf, raw_cap);
            if (!exp) {
                free(raw_buf);
                close(sock);
                return -1;
            }
            raw_buf = exp;
        }

        ssize_t n = recv(sock, raw_buf + raw_len, 2048, 0);
        if (n <= 0) {
            break; // Connection closed or error before headers completed
        }
        raw_len += (size_t)n;
        raw_buf[raw_len] = '\0';
        header_end = strstr(raw_buf, "\r\n\r\n");
    }

    if (!header_end) {
        free(raw_buf);
        close(sock);
        return -1;
    }

    size_t header_len = (size_t)(header_end - raw_buf) + 4;
    size_t initial_body_bytes = raw_len - header_len;

    // Parse status line: HTTP/1.x <code> <msg>
    char* line = raw_buf;
    char* next_line = strstr(line, "\r\n");
    if (!next_line) {
        free(raw_buf);
        close(sock);
        return -1;
    }
    *next_line = '\0';

    int status_code = 0;
    char status_msg[64] = {0};
    if (sscanf(line, "HTTP/%*d.%*d %d %63[^\r\n]", &status_code, status_msg) < 1) {
        free(raw_buf);
        close(sock);
        return -1;
    }
    resp->status_code = status_code;
    strncpy(resp->status_message, status_msg, sizeof(resp->status_message) - 1);

    // Parse headers
    line = next_line + 2;
    while (line < header_end) {
        next_line = strstr(line, "\r\n");
        if (!next_line) break;
        *next_line = '\0';

        char* colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char* h_name = trim_whitespace(line);
            char* h_val = trim_whitespace(colon + 1);
            if (h_name && *h_name) {
                add_response_header(resp, h_name, h_val);
            }
        }
        line = next_line + 2;
    }

    // Read response body
    const char* cl_header = http_response_get_header(resp, "Content-Length");
    if (cl_header) {
        size_t content_length = (size_t)strtoull(cl_header, NULL, 10);
        resp->body = (char*)malloc(content_length + 1);
        if (!resp->body) {
            free(raw_buf);
            close(sock);
            return -1;
        }

        size_t copied = (initial_body_bytes > content_length) ? content_length : initial_body_bytes;
        if (copied > 0) {
            memcpy(resp->body, header_end + 4, copied);
        }
        resp->body_len = copied;

        while (resp->body_len < content_length) {
            ssize_t n = recv(sock, resp->body + resp->body_len, content_length - resp->body_len, 0);
            if (n <= 0) break;
            resp->body_len += (size_t)n;
        }
        resp->body[resp->body_len] = '\0';
    } else {
        // Read until EOF
        size_t b_cap = initial_body_bytes + 4096;
        resp->body = (char*)malloc(b_cap);
        if (!resp->body) {
            free(raw_buf);
            close(sock);
            return -1;
        }
        if (initial_body_bytes > 0) {
            memcpy(resp->body, header_end + 4, initial_body_bytes);
        }
        resp->body_len = initial_body_bytes;

        while (1) {
            if (resp->body_len + 2048 >= b_cap) {
                b_cap *= 2;
                char* exp = (char*)realloc(resp->body, b_cap);
                if (!exp) break;
                resp->body = exp;
            }
            ssize_t n = recv(sock, resp->body + resp->body_len, 2048, 0);
            if (n <= 0) break;
            resp->body_len += (size_t)n;
        }
        resp->body[resp->body_len] = '\0';
    }

    free(raw_buf);
    close(sock);
    return 0;
}
