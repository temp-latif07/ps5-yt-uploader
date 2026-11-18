#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "api.h"
#include "config.h"
#include "watcher.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define TEST_PORT 8889
#define TEST_CONFIG_DIR "/tmp/ps5_test_server_storage"
#define TEST_CAPTURES_DIR "/tmp/ps5_test_server_captures"

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

static void rm_rf(const char* path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

static void create_dummy_file(const char* path, const char* content, size_t len) {
    FILE* fp = fopen(path, "wb");
    assert(fp != NULL);
    if (len > 0) {
        size_t written = fwrite(content, 1, len, fp);
        assert(written == len);
    }
    fclose(fp);
}

/**
 * Raw HTTP client helper using BSD sockets to test actual wire protocol.
 */
static int http_request(int port, const char* method, const char* path,
                         const char* body, const char* extra_headers,
                         int* out_status, char* out_headers, size_t max_headers,
                         char* out_body, size_t max_body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 2;
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

    char req[8192];
    size_t body_len = body ? strlen(body) : 0;
    int req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "User-Agent: test-client/1.0\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        method, path, port, body_len,
        extra_headers ? extra_headers : "",
        body ? body : "");

    if (send(sock, req, req_len, 0) != req_len) {
        close(sock);
        return -1;
    }

    char response[65536];
    size_t total_recv = 0;
    while (total_recv < sizeof(response) - 1) {
        ssize_t n = recv(sock, response + total_recv, sizeof(response) - 1 - total_recv, 0);
        if (n <= 0) {
            break;
        }
        total_recv += n;
    }
    response[total_recv] = '\0';
    close(sock);

    if (total_recv == 0) {
        return -1;
    }

    // Parse status line
    int status = 0;
    if (sscanf(response, "HTTP/1.1 %d", &status) != 1 &&
        sscanf(response, "HTTP/1.0 %d", &status) != 1) {
        return -1;
    }
    if (out_status) {
        *out_status = status;
    }

    char* header_end = strstr(response, "\r\n\r\n");
    if (!header_end) {
        header_end = strstr(response, "\n\n");
    }

    if (header_end) {
        size_t hlen = (size_t)(header_end - response);
        if (out_headers) {
            snprintf(out_headers, max_headers, "%.*s", (int)hlen, response);
        }
        char* bstart = header_end + (strstr(response, "\r\n\r\n") ? 4 : 2);
        if (out_body) {
            snprintf(out_body, max_body, "%s", bstart);
        }
    } else {
        if (out_headers) out_headers[0] = '\0';
        if (out_body) out_body[0] = '\0';
    }

    return 0;
}

static void test_cors_options_preflight(void) {
    printf("[Test 1] Testing OPTIONS /api/status CORS preflight...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[1024] = {0};

    int rc = http_request(TEST_PORT, "OPTIONS", "/api/status", NULL,
                          "Origin: http://192.168.1.50:3000\r\n"
                          "Access-Control-Request-Method: GET\r\n",
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200 || status == 204);

    // Verify CORS headers
    assert(ci_strstr(headers, "Access-Control-Allow-Origin: *") != NULL);
    assert(ci_strstr(headers, "Access-Control-Allow-Methods:") != NULL);
    assert(ci_strstr(headers, "OPTIONS") != NULL);
    printf("  -> CORS preflight OPTIONS passed with HTTP %d and proper headers!\n", status);
}

static void test_get_api_status_initial(void) {
    printf("[Test 2] Testing initial GET /api/status...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[4096] = {0};

    int rc = http_request(TEST_PORT, "GET", "/api/status", NULL, NULL,
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);
    assert(ci_strstr(headers, "Access-Control-Allow-Origin: *") != NULL);
    assert(ci_strstr(headers, "application/json") != NULL);

    cJSON* root = cJSON_Parse(body);
    assert(root != NULL);
    assert(cJSON_IsObject(root));

    cJSON* auth = cJSON_GetObjectItem(root, "authenticated");
    assert(auth != NULL && cJSON_IsBool(auth));
    assert(cJSON_IsFalse(auth)); // Initially unauthenticated

    cJSON* ch_name = cJSON_GetObjectItem(root, "channel_name");
    assert(ch_name != NULL && cJSON_IsString(ch_name));

    cJSON* ch_title = cJSON_GetObjectItem(root, "channel_title");
    assert(ch_title != NULL && cJSON_IsString(ch_title));

    cJSON* auto_up = cJSON_GetObjectItem(root, "auto_upload");
    assert(auto_up != NULL && cJSON_IsBool(auto_up));

    cJSON* priv = cJSON_GetObjectItem(root, "default_privacy");
    assert(priv != NULL && cJSON_IsString(priv));
    assert(strcmp(priv->valuestring, "UNLISTED") == 0);

    cJSON_Delete(root);
    printf("  -> Initial GET /api/status passed! Body: %s\n", body);
}

static void test_get_api_clips(void) {
    printf("[Test 3] Testing GET /api/clips...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[8192] = {0};

    int rc = http_request(TEST_PORT, "GET", "/api/clips", NULL, NULL,
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);
    assert(ci_strstr(headers, "Access-Control-Allow-Origin: *") != NULL);

    cJSON* root = cJSON_Parse(body);
    assert(root != NULL);
    assert(cJSON_IsArray(root));

    int count = cJSON_GetArraySize(root);
    assert(count == 2);

    cJSON* item0 = cJSON_GetArrayItem(root, 0);
    assert(cJSON_IsObject(item0));
    assert(cJSON_GetObjectItem(item0, "filepath") != NULL);
    assert(cJSON_GetObjectItem(item0, "title_id") != NULL);
    assert(cJSON_GetObjectItem(item0, "timestamp") != NULL);
    assert(cJSON_GetObjectItem(item0, "size_bytes") != NULL);

    cJSON_Delete(root);
    printf("  -> GET /api/clips returned %d clips successfully!\n", count);
}

static void test_post_api_config(void) {
    printf("[Test 4] Testing POST /api/config...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[4096] = {0};

    const char* req_json = "{\"auto_upload\":false,\"default_privacy\":\"PUBLIC\"}";
    int rc = http_request(TEST_PORT, "POST", "/api/config", req_json,
                          "Content-Type: application/json\r\n",
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);

    cJSON* root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* succ = cJSON_GetObjectItem(root, "success");
    assert(succ != NULL && cJSON_IsTrue(succ));
    cJSON_Delete(root);

    // Verify config was persisted by calling GET /api/status
    memset(body, 0, sizeof(body));
    rc = http_request(TEST_PORT, "GET", "/api/status", NULL, NULL,
                      &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);
    root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* auto_up = cJSON_GetObjectItem(root, "auto_upload");
    assert(auto_up && cJSON_IsFalse(auto_up));
    cJSON* priv = cJSON_GetObjectItem(root, "default_privacy");
    assert(priv && strcmp(priv->valuestring, "PUBLIC") == 0);
    cJSON_Delete(root);

    printf("  -> POST /api/config updated and verified successfully!\n");
}

static void test_post_api_auth(void) {
    printf("[Test 5] Testing POST /api/auth...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[4096] = {0};

    const char* req_json = "{\"cookies\":\"SID=test_sid_123; HSID=test_hsid_456\","
                           "\"session_token\":\"test_sess_token_789\","
                           "\"channel_name\":\"My PS5 Channel\","
                           "\"channel_id\":\"UC1234567890\"}";
    int rc = http_request(TEST_PORT, "POST", "/api/auth", req_json,
                          "Content-Type: application/json\r\n",
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);

    cJSON* root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* succ = cJSON_GetObjectItem(root, "success");
    assert(succ != NULL && cJSON_IsTrue(succ));
    cJSON* ch_name = cJSON_GetObjectItem(root, "channel_name");
    assert(ch_name != NULL && strcmp(ch_name->valuestring, "My PS5 Channel") == 0);
    cJSON* ch_title = cJSON_GetObjectItem(root, "channel_title");
    assert(ch_title != NULL && strcmp(ch_title->valuestring, "My PS5 Channel") == 0);
    cJSON_Delete(root);

    // Verify GET /api/status now reports authenticated: true
    memset(body, 0, sizeof(body));
    rc = http_request(TEST_PORT, "GET", "/api/status", NULL, NULL,
                      &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);
    root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* auth = cJSON_GetObjectItem(root, "authenticated");
    assert(auth && cJSON_IsTrue(auth));
    cJSON_Delete(root);

    printf("  -> POST /api/auth authenticated and saved credentials successfully!\n");
}

static void test_post_api_upload(void) {
    printf("[Test 6] Testing POST /api/upload...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[4096] = {0};

    char clip_path[512];
    snprintf(clip_path, sizeof(clip_path), "%s/CUSA00001/clip1.mp4", TEST_CAPTURES_DIR);

    char req_json[1024];
    snprintf(req_json, sizeof(req_json),
             "{\"filepath\":\"%s\",\"title\":\"Boss Fight Clip\",\"description\":\"PS5 Victory\",\"privacy\":\"UNLISTED\"}",
             clip_path);

    int rc = http_request(TEST_PORT, "POST", "/api/upload", req_json,
                          "Content-Type: application/json\r\n",
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);

    cJSON* root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* st_item = cJSON_GetObjectItem(root, "status");
    assert(st_item != NULL && strcmp(st_item->valuestring, "queued") == 0);
    cJSON_Delete(root);

    printf("  -> POST /api/upload queued clip successfully!\n");
}

static void test_not_found_route(void) {
    printf("[Test 7] Testing 404 for unknown route...\n");
    int status = 0;
    char headers[2048] = {0};
    char body[1024] = {0};

    int rc = http_request(TEST_PORT, "GET", "/api/nonexistent", NULL, NULL,
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 404);
    printf("  -> Unknown route returned 404 Not Found as expected!\n");
}

static void test_oversized_content_length(void) {
    printf("[Test 8] Testing oversized Content-Length (>64KB) rejection...\n");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    const char* raw_req =
        "POST /api/config HTTP/1.1\r\n"
        "Host: 127.0.0.1:8889\r\n"
        "Content-Length: 100000\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"test\":1}";
    send(sock, raw_req, strlen(raw_req), 0);

    char resp[4096] = {0};
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);

    assert(n > 0);
    int status = 0;
    sscanf(resp, "HTTP/1.1 %d", &status);
    assert(status == 413 || status == 400);
    printf("  -> Oversized Content-Length safely rejected with HTTP %d without memory corruption!\n", status);
}

static void test_client_disconnect_and_sigpipe(void) {
    printf("[Test 9] Testing premature disconnect and broken pipe SIGPIPE resilience...\n");

    // Case 1: Connect and send partial body, then abrupt close
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    const char* partial_req =
        "POST /api/upload HTTP/1.1\r\n"
        "Host: 127.0.0.1:8889\r\n"
        "Content-Length: 1024\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
        "{\"filepath\":\"partial";
    send(sock, partial_req, strlen(partial_req), 0);

    struct linger sl = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(sock);

    usleep(30000); // 30ms

    // Case 2: Connect, send full GET request, but close immediately so server sends response into closed socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);
    assert(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    const char* get_req = "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1:8889\r\nConnection: close\r\n\r\n";
    send(sock, get_req, strlen(get_req), 0);
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(sock);

    usleep(50000); // 50ms

    // Verify daemon is still alive and answering requests
    int status = 0;
    char headers[2048] = {0};
    char body[1024] = {0};
    int rc = http_request(TEST_PORT, "GET", "/api/status", NULL, NULL,
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);
    printf("  -> Server survived premature disconnect and broken pipe without SIGPIPE termination!\n");
}

static void test_upload_concurrency_limit(void) {
    printf("[Test 10] Testing POST /api/upload concurrency limit rejection...\n");

    char clip_path[512];
    snprintf(clip_path, sizeof(clip_path), "%s/CUSA00001/clip1.mp4", TEST_CAPTURES_DIR);

    char req_json[1024];
    snprintf(req_json, sizeof(req_json),
             "{\"filepath\":\"%s\",\"title\":\"Concurrency Test\",\"privacy\":\"UNLISTED\"}",
             clip_path);

    // Set delay to 150ms so threads remain active concurrently
    api_set_upload_delay_ms(150);

    int status = 0;
    char headers[2048] = {0};
    char body[4096] = {0};

    // Fill the concurrency limit (API_MAX_CONCURRENT_UPLOADS = 4)
    for (int i = 0; i < API_MAX_CONCURRENT_UPLOADS; i++) {
        memset(body, 0, sizeof(body));
        int rc = http_request(TEST_PORT, "POST", "/api/upload", req_json,
                              "Content-Type: application/json\r\n",
                              &status, headers, sizeof(headers), body, sizeof(body));
        assert(rc == 0);
        assert(status == 200);
        cJSON* root = cJSON_Parse(body);
        assert(root != NULL);
        cJSON* st = cJSON_GetObjectItem(root, "status");
        assert(st != NULL && strcmp(st->valuestring, "queued") == 0);
        cJSON_Delete(root);
    }

    // 5th request must be rejected due to max concurrent uploads
    memset(body, 0, sizeof(body));
    int rc = http_request(TEST_PORT, "POST", "/api/upload", req_json,
                          "Content-Type: application/json\r\n",
                          &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    cJSON* root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* succ = cJSON_GetObjectItem(root, "success");
    assert(succ != NULL && cJSON_IsFalse(succ));
    cJSON* err = cJSON_GetObjectItem(root, "error");
    assert(err != NULL && strstr(err->valuestring, "concurrent") != NULL);
    cJSON_Delete(root);

    // Wait for uploads to complete
    api_set_upload_delay_ms(0);
    usleep(200000); // 200ms

    // After workers finish, new uploads should be accepted again
    memset(body, 0, sizeof(body));
    rc = http_request(TEST_PORT, "POST", "/api/upload", req_json,
                      "Content-Type: application/json\r\n",
                      &status, headers, sizeof(headers), body, sizeof(body));
    assert(rc == 0);
    assert(status == 200);
    root = cJSON_Parse(body);
    assert(root != NULL);
    cJSON* succ2 = cJSON_GetObjectItem(root, "success");
    assert(succ2 != NULL && cJSON_IsTrue(succ2));
    cJSON_Delete(root);

    printf("  -> Concurrency limit properly enforced and rejected 5th upload with error!\n");
}

int main(void) {
    rm_rf(TEST_CONFIG_DIR);
    rm_rf(TEST_CAPTURES_DIR);

    mkdir(TEST_CONFIG_DIR, 0777);
    mkdir(TEST_CAPTURES_DIR, 0777);

    // Setup mock captures
    char game_dir[512];
    snprintf(game_dir, sizeof(game_dir), "%s/CUSA00001", TEST_CAPTURES_DIR);
    mkdir(game_dir, 0777);

    char clip1[512], clip2[512];
    snprintf(clip1, sizeof(clip1), "%s/clip1.mp4", game_dir);
    snprintf(clip2, sizeof(clip2), "%s/clip2.mp4", game_dir);
    create_dummy_file(clip1, "clip1_video_data_sample", 24);
    create_dummy_file(clip2, "clip2_video_data_sample_longer", 31);

    // Initialize subsystems
    assert(config_init(TEST_CONFIG_DIR) == 0);
    assert(api_init(TEST_CAPTURES_DIR) == 0);

    printf("Starting HTTP server on port %d...\n", TEST_PORT);
    assert(server_start(TEST_PORT) == 0);
    usleep(50000); // 50ms to ensure listener is active

    test_cors_options_preflight();
    test_get_api_status_initial();
    test_get_api_clips();
    test_post_api_config();
    test_post_api_auth();
    test_post_api_upload();
    test_not_found_route();
    test_oversized_content_length();
    test_client_disconnect_and_sigpipe();
    test_upload_concurrency_limit();

    printf("Stopping HTTP server...\n");
    server_stop();
    server_clear_routes();
    api_cleanup();

    // Verify server has stopped by asserting connection failure
    int test_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int conn_rc = connect(test_sock, (struct sockaddr*)&addr, sizeof(addr));
    assert(conn_rc != 0); // Must fail
    close(test_sock);
    printf("  -> Verified server port %d closed cleanly!\n", TEST_PORT);

    // Cleanup directories
    rm_rf(TEST_CONFIG_DIR);
    rm_rf(TEST_CAPTURES_DIR);

    printf("Task 5 Server API Tests Passed Successfully!\n");
    return 0;
}
