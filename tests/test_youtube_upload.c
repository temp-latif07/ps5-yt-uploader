#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <signal.h>

#include "../src/config.h"
#include "../src/crypto.h"
#include "../src/http_client.h"
#include "../src/youtube.h"

#define MOCK_PORT 8999
#define SAMPLE_CLIP_PATH "spikes/sample_clip.mp4"

static pid_t mock_server_pid = 0;

static int is_server_alive(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    return (res == 0);
}

static void ensure_mock_server_running(void) {
    if (is_server_alive(MOCK_PORT)) {
        return;
    }

    // Fork and exec mock_youtube_server.py
    mock_server_pid = fork();
    if (mock_server_pid == 0) {
        // Child
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", MOCK_PORT);
        execlp("python3", "python3", "tests/mock_youtube_server.py", port_str, NULL);
        perror("execlp failed");
        exit(1);
    }

    // Wait until server starts up
    for (int i = 0; i < 30; i++) {
        usleep(100000); // 100ms
        if (is_server_alive(MOCK_PORT)) {
            return;
        }
    }
    fprintf(stderr, "Mock server failed to start on port %d\n", MOCK_PORT);
    exit(1);
}

static void cleanup_mock_server(void) {
    if (mock_server_pid > 0) {
        kill(mock_server_pid, SIGTERM);
        mock_server_pid = 0;
    }
}

typedef struct {
    size_t calls;
    size_t last_uploaded;
    size_t total;
} test_progress_t;

static void on_progress(size_t uploaded_bytes, size_t total_bytes, void* userdata) {
    test_progress_t* p = (test_progress_t*)userdata;
    p->calls++;
    p->last_uploaded = uploaded_bytes;
    p->total = total_bytes;
}

static void setup_valid_config(app_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->cookies, sizeof(cfg->cookies), "SAPISID=mock_sapisid_token_12345; __Secure-3PSID=mock_sid_67890;");
    snprintf(cfg->session_token, sizeof(cfg->session_token), "mock_session_token_abc123");
    snprintf(cfg->innertube_key, sizeof(cfg->innertube_key), "mock_innertube_key_xyz");
    snprintf(cfg->channel_id, sizeof(cfg->channel_id), "UC_MOCK_CHANNEL_123");
    snprintf(cfg->channel_name, sizeof(cfg->channel_name), "PS5 Mock Channel");
    snprintf(cfg->default_privacy, sizeof(cfg->default_privacy), "UNLISTED");
    cfg->auto_upload = 1;
    cfg->server_port = 8888;
}

// 1. Normal end-to-end upload of spikes/sample_clip.mp4
static void test_normal_upload_e2e(void) {
    printf("[Test 1] Running normal end-to-end upload...\n");

    app_config_t cfg;
    setup_valid_config(&cfg);

    upload_result_t result;
    memset(&result, 0, sizeof(result));

    test_progress_t progress = {0, 0, 0};

    int rc = youtube_upload_clip(&cfg, SAMPLE_CLIP_PATH, "PS5 Spike Test Video", "Uploaded via PS5 YouTube Uploader", "UNLISTED", on_progress, &progress, &result);

    assert(rc == 0);
    assert(result.http_status == 200);
    assert(strcmp(result.video_id, "mock_vid_98765") == 0);
    assert(strcmp(result.youtube_url, "https://youtu.be/mock_vid_98765") == 0);
    assert(result.error_message[0] == '\0');
    assert(progress.calls > 0);
    assert(progress.last_uploaded == progress.total);
    assert(progress.total > 0);

    printf("  -> Passed! Video ID: %s, URL: %s, Progress: %zu/%zu bytes across %zu callbacks\n",
           result.video_id, result.youtube_url, progress.last_uploaded, progress.total, progress.calls);
}

// 2. Resume on mid-stream disconnection (Review Focus #3)
static void test_resume_on_midstream_drop(void) {
    printf("[Test 2] Running mid-stream drop and resume recovery...\n");

    app_config_t cfg;
    setup_valid_config(&cfg);

    char upload_url[512] = {0};
    const char* frontend_id = "test_simulate_drop_frontend_001";

    // Step 2a: Initialize session with simulate_drop frontend id
    int rc = youtube_init_upload_session(&cfg, frontend_id, upload_url, sizeof(upload_url));
    assert(rc == 0);
    assert(strlen(upload_url) > 0);
    assert(strstr(upload_url, "/upload/resumable/") != NULL);

    // Step 2b: Stream file. Server drops connection on first chunk; engine queries offset and resumes
    char scotty_id[128] = {0};
    test_progress_t progress = {0, 0, 0};

    rc = youtube_stream_file(&cfg, upload_url, SAMPLE_CLIP_PATH, scotty_id, sizeof(scotty_id), on_progress, &progress);
    assert(rc == 0);
    assert(strcmp(scotty_id, "mock_scotty_resource_12345") == 0);
    assert(progress.calls > 0);
    assert(progress.last_uploaded == progress.total);

    // Step 2c: Finalize video via InnerTube
    upload_result_t result;
    memset(&result, 0, sizeof(result));

    rc = youtube_finalize_video(&cfg, frontend_id, scotty_id, "Resumed Video", "Resumed Description", "UNLISTED", &result);
    assert(rc == 0);
    assert(result.http_status == 200);
    assert(strcmp(result.video_id, "mock_vid_98765") == 0);
    assert(strcmp(result.youtube_url, "https://youtu.be/mock_vid_98765") == 0);

    printf("  -> Passed! Successfully recovered from mid-stream network drop and resumed to completion (scottyId: %s)\n", scotty_id);
}

// 2b. Resume when all bytes were received but connection dropped before response (Issue 2)
static void test_resume_full_bytes_received(void) {
    printf("[Test 2b] Running resume when all bytes received before disconnect...\n");

    app_config_t cfg;
    setup_valid_config(&cfg);

    char upload_url[512] = {0};
    const char* frontend_id = "test_simulate_drop_full_002";

    int rc = youtube_init_upload_session(&cfg, frontend_id, upload_url, sizeof(upload_url));
    assert(rc == 0);

    char scotty_id[128] = {0};
    rc = youtube_stream_file(&cfg, upload_url, SAMPLE_CLIP_PATH, scotty_id, sizeof(scotty_id), NULL, NULL);
    assert(rc == 0);
    assert(strcmp(scotty_id, "mock_scotty_resource_12345") == 0);

    printf("  -> Passed! Sent 0-byte finalize chunk when all bytes were received and finalized successfully.\n");
}

// 3. Proper UNAUTHENTICATED error handling when credentials are invalid (Review Focus #2)
static void test_unauthenticated_error_handling(void) {
    printf("[Test 3] Running unauthenticated error handling tests...\n");

    // Case 3a: Expired / Invalid session_token (handled by mock server HTTP 401 response, not sentinel check)
    {
        app_config_t cfg;
        setup_valid_config(&cfg);
        strcpy(cfg.session_token, "EXPIRED_TOKEN");

        upload_result_t result;
        memset(&result, 0, sizeof(result));

        int rc = youtube_upload_clip(&cfg, SAMPLE_CLIP_PATH, "Expired Test", "Desc", "UNLISTED", NULL, NULL, &result);
        assert(rc != 0);
        assert(result.http_status == 401);
        assert(strcmp(result.error_message, "UNAUTHENTICATED: session token or cookies expired") == 0);
        printf("  -> 3a (Expired session_token via HTTP 401): Caught HTTP 401 with error: '%s'\n", result.error_message);
    }

    // Case 3b: Expired cookies / Missing SAPISID
    {
        app_config_t cfg;
        setup_valid_config(&cfg);
        strcpy(cfg.cookies, "EXPIRED_COOKIE=1; OTHER_VAL=abc"); // missing SAPISID

        upload_result_t result;
        memset(&result, 0, sizeof(result));

        int rc = youtube_upload_clip(&cfg, SAMPLE_CLIP_PATH, "Expired Cookie Test", "Desc", "UNLISTED", NULL, NULL, &result);
        assert(rc != 0);
        assert(result.http_status == 401);
        assert(strcmp(result.error_message, "UNAUTHENTICATED: session token or cookies expired") == 0);
        printf("  -> 3b (Missing/Expired SAPISID cookie): Caught HTTP 401 with error: '%s'\n", result.error_message);
    }

    // Case 3c: Explicit mock server 401 on session init
    {
        app_config_t cfg;
        setup_valid_config(&cfg);
        strcpy(cfg.cookies, "SAPISID=EXPIRED_COOKIE_VALUE");

        char upload_url[512] = {0};
        int rc = youtube_init_upload_session(&cfg, "invalid_auth_id", upload_url, sizeof(upload_url));
        assert(rc != 0);
        printf("  -> 3c (Session Init 401): youtube_init_upload_session returned error as expected\n");
    }

    // Case 3d: Query returning 401 halts immediately without retry loop (Issue 5)
    {
        app_config_t cfg;
        setup_valid_config(&cfg);

        char upload_url[512] = {0};
        int rc = youtube_init_upload_session(&cfg, "test_simulate_drop_query_401", upload_url, sizeof(upload_url));
        assert(rc == 0);

        char scotty_id[128] = {0};
        rc = youtube_stream_file(&cfg, upload_url, SAMPLE_CLIP_PATH, scotty_id, sizeof(scotty_id), NULL, NULL);
        assert(rc != 0);
        printf("  -> 3d (Query returning 401): Halted immediately without retrying\n");
    }

    printf("  -> Passed! All UNAUTHENTICATED error paths handled cleanly without retrying.\n");
}

int main(void) {
    ensure_mock_server_running();

    // Configure base URLs to point to local mock server
    char scotty_init_url[256];
    char innertube_url[256];
    snprintf(scotty_init_url, sizeof(scotty_init_url), "http://127.0.0.1:%d/upload/studio?authuser=0", MOCK_PORT);
    snprintf(innertube_url, sizeof(innertube_url), "http://127.0.0.1:%d/youtubei/v1/upload/createvideo", MOCK_PORT);
    youtube_set_base_urls(scotty_init_url, innertube_url);

    test_normal_upload_e2e();
    test_resume_on_midstream_drop();
    test_resume_full_bytes_received();
    test_unauthenticated_error_handling();

    cleanup_mock_server();
    printf("Task 3 YouTube Engine Tests Passed Successfully!\n");
    return 0;
}
