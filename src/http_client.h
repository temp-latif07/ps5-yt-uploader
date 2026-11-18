#ifndef PS5_YOUTUBE_HTTP_CLIENT_H
#define PS5_YOUTUBE_HTTP_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[128];
    char* value;
} http_header_t;

typedef struct {
    int status_code;
    char status_message[64];
    http_header_t* headers;
    size_t header_count;
    size_t header_capacity;
    char* body;
    size_t body_len;
} http_response_t;

void http_response_init(http_response_t* resp);
void http_response_cleanup(http_response_t* resp);
const char* http_response_get_header(const http_response_t* resp, const char* name);

typedef void (*http_progress_cb)(size_t sent_bytes, size_t total_bytes, void* userdata);

typedef struct {
    const char* method;            // "GET", "POST", "PUT"
    const char* url;               // "http://127.0.0.1:8999/upload/studio?authuser=0"
    const http_header_t* headers;
    size_t header_count;

    // In-memory body (optional)
    const void* body;
    size_t body_len;

    // File streaming body (optional, used if body_filepath != NULL)
    const char* body_filepath;
    size_t file_offset;
    size_t file_length;            // 0 means entire file from file_offset

    // Progress callback
    http_progress_cb progress_cb;
    void* userdata;

    int timeout_sec;               // Connection and I/O timeout in seconds
} http_request_t;

/**
 * Executes an HTTP request synchronously over BSD sockets.
 * Handles request construction, body chunking / streaming, and response header parsing.
 *
 * @param req  Request descriptor
 * @param resp Response descriptor to populate (must be initialized via http_response_init)
 * @return 0 on successful HTTP transaction (status code received), -1 on network/I/O error
 */
int http_client_request(const http_request_t* req, http_response_t* resp);

#ifdef __cplusplus
}
#endif

#endif /* PS5_YOUTUBE_HTTP_CLIENT_H */
