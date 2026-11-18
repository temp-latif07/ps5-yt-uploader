#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <curl/curl.h>

static pthread_once_t g_curl_init_once = PTHREAD_ONCE_INIT;

static void curl_global_init_routine(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

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

typedef struct {
    FILE* fp;
    size_t remaining;
    size_t sent;
    size_t file_offset;
    size_t total_file_size;
    http_progress_cb progress_cb;
    void* userdata;
} file_read_ctx_t;

static size_t curl_read_file_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    file_read_ctx_t* ctx = (file_read_ctx_t*)userdata;
    if (!ctx || ctx->remaining == 0 || !ctx->fp) return 0;

    size_t max_to_read = size * nitems;
    if (max_to_read > ctx->remaining) {
        max_to_read = ctx->remaining;
    }

    size_t bytes_read = fread(buffer, 1, max_to_read, ctx->fp);
    if (bytes_read > 0) {
        ctx->remaining -= bytes_read;
        ctx->sent += bytes_read;
        if (ctx->progress_cb) {
            ctx->progress_cb(ctx->file_offset + ctx->sent, ctx->total_file_size, ctx->userdata);
        }
    }
    return bytes_read;
}

static size_t curl_write_body_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_response_t* resp = (http_response_t*)userdata;
    size_t total_bytes = size * nmemb;
    if (total_bytes == 0) return 0;

    char* new_body = (char*)realloc(resp->body, resp->body_len + total_bytes + 1);
    if (!new_body) return 0;

    resp->body = new_body;
    memcpy(resp->body + resp->body_len, ptr, total_bytes);
    resp->body_len += total_bytes;
    resp->body[resp->body_len] = '\0';
    return total_bytes;
}

static size_t curl_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    http_response_t* resp = (http_response_t*)userdata;
    size_t total_len = size * nitems;
    if (total_len == 0) return 0;

    char line[4096];
    size_t copy_len = (total_len < sizeof(line) - 1) ? total_len : (sizeof(line) - 1);
    memcpy(line, buffer, copy_len);
    line[copy_len] = '\0';

    while (copy_len > 0 && (line[copy_len - 1] == '\r' || line[copy_len - 1] == '\n')) {
        line[copy_len - 1] = '\0';
        copy_len--;
    }

    if (strncmp(line, "HTTP/", 5) == 0) {
        int code = 0;
        char msg[64] = {0};
        if (sscanf(line, "HTTP/%*d.%*d %d %63[^\r\n]", &code, msg) >= 1) {
            resp->status_code = code;
            strncpy(resp->status_message, msg, sizeof(resp->status_message) - 1);
        }
    } else {
        char* colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char* h_name = trim_whitespace(line);
            char* h_val = trim_whitespace(colon + 1);
            if (h_name && *h_name) {
                add_response_header(resp, h_name, h_val);
            }
        }
    }
    return total_len;
}

int http_client_request(const http_request_t* req, http_response_t* resp) {
    if (!req || !resp || !req->url || !req->method) return -1;

    pthread_once(&g_curl_init_once, curl_global_init_routine);

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    // Build headers list
    struct curl_slist* hlist = NULL;
    if (req->headers) {
        for (size_t i = 0; i < req->header_count; i++) {
            char hline[2048];
            snprintf(hline, sizeof(hline), "%s: %s",
                     req->headers[i].name,
                     req->headers[i].value ? req->headers[i].value : "");
            hlist = curl_slist_append(hlist, hline);
        }
    }
    // Disable Expect: 100-continue
    hlist = curl_slist_append(hlist, "Expect:");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

    int timeout_sec = (req->timeout_sec > 0) ? req->timeout_sec : 15;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    // TLS verification for HTTPS
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Body streaming setup
    file_read_ctx_t file_ctx;
    memset(&file_ctx, 0, sizeof(file_ctx));

    int is_post = (strcasecmp(req->method, "POST") == 0);
    int is_put = (strcasecmp(req->method, "PUT") == 0);
    int is_get = (strcasecmp(req->method, "GET") == 0);

    if (is_get) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (is_post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (is_put) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
    }

    if (req->body_filepath) {
        struct stat st;
        if (stat(req->body_filepath, &st) != 0) {
            curl_slist_free_all(hlist);
            curl_easy_cleanup(curl);
            return -1;
        }

        size_t file_size = (size_t)st.st_size;
        size_t content_len = 0;
        if (req->file_length > 0) {
            content_len = req->file_length;
        } else if (file_size >= req->file_offset) {
            content_len = file_size - req->file_offset;
        }

        FILE* fp = fopen(req->body_filepath, "rb");
        if (!fp) {
            curl_slist_free_all(hlist);
            curl_easy_cleanup(curl);
            return -1;
        }

        if (req->file_offset > 0) {
            if (fseek(fp, (long)req->file_offset, SEEK_SET) != 0) {
                fclose(fp);
                curl_slist_free_all(hlist);
                curl_easy_cleanup(curl);
                return -1;
            }
        }

        file_ctx.fp = fp;
        file_ctx.remaining = content_len;
        file_ctx.sent = 0;
        file_ctx.file_offset = req->file_offset;
        file_ctx.total_file_size = file_size;
        file_ctx.progress_cb = req->progress_cb;
        file_ctx.userdata = req->userdata;

        if (is_post) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)content_len);
        } else {
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)content_len);
        }
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, curl_read_file_cb);
        curl_easy_setopt(curl, CURLOPT_READDATA, &file_ctx);
    } else if (req->body && req->body_len > 0) {
        if (is_post) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
        } else if (is_put) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)req->body_len);
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
        }
    } else if (is_post) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
    }

    CURLcode res = curl_easy_perform(curl);

    if (file_ctx.fp) {
        fclose(file_ctx.fp);
        file_ctx.fp = NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (resp->status_code == 0 && http_code > 0) {
        resp->status_code = (int)http_code;
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return -1;
    }

    // Check premature connection termination (Review Focus: Important 4)
    const char* cl_header = http_response_get_header(resp, "Content-Length");
    if (cl_header) {
        size_t expected_len = (size_t)strtoull(cl_header, NULL, 10);
        if (resp->body_len < expected_len) {
            return -1; // Truncated response body
        }
    }

    if (req->body && req->body_len > 0 && req->progress_cb) {
        req->progress_cb(req->body_len, req->body_len, req->userdata);
    }

    return 0;
}
