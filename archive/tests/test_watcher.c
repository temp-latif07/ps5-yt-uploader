#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#include <dirent.h>
#include <pthread.h>

#include "watcher.h"
#include "notify.h"
#include "config.h"
#include "youtube.h"

static const char* TEST_ROOT = "/tmp/test_ps5_watcher_captures";
static const char* TEST_CONFIG_DIR = "/tmp/test_ps5_watcher_config";

static void rm_rf(const char* path) {
    DIR* d = opendir(path);
    if (!d) {
        unlink(path);
        rmdir(path);
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, entry->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                rm_rf(sub);
            } else {
                unlink(sub);
            }
        }
    }
    closedir(d);
    rmdir(path);
}

static void create_file(const char* filepath, const char* data, size_t size, time_t mtime) {
    FILE* f = fopen(filepath, "wb");
    assert(f != NULL);
    if (size > 0 && data != NULL) {
        size_t written = fwrite(data, 1, size, f);
        assert(written == size);
    }
    fclose(f);
    if (mtime > 0) {
        struct utimbuf ut;
        ut.actime = mtime;
        ut.modtime = mtime;
        utime(filepath, &ut);
    }
}

static void append_file(const char* filepath, const char* data, size_t size) {
    FILE* f = fopen(filepath, "ab");
    assert(f != NULL);
    if (size > 0 && data != NULL) {
        size_t written = fwrite(data, 1, size, f);
        assert(written == size);
    }
    fclose(f);
}

typedef struct {
    capture_clip_t clips[32];
    size_t count;
    pthread_mutex_t mutex;
} test_watcher_ctx_t;

static void test_clip_ready_cb(const capture_clip_t* clip, void* userdata) {
    test_watcher_ctx_t* ctx = (test_watcher_ctx_t*)userdata;
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->count < 32) {
        ctx->clips[ctx->count] = *clip;
        ctx->count++;
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static char s_last_notification[512] = {0};
static int s_notification_count = 0;
static pthread_mutex_t s_notify_test_mutex = PTHREAD_MUTEX_INITIALIZER;

static void test_notify_spy(const char* msg) {
    pthread_mutex_lock(&s_notify_test_mutex);
    snprintf(s_last_notification, sizeof(s_last_notification), "%s", msg);
    s_notification_count++;
    pthread_mutex_unlock(&s_notify_test_mutex);
}

static void test_scan_existing_clips(void) {
    printf("[Test 1] Testing scan_existing_clips sorting and file discovery...\n");
    rm_rf(TEST_ROOT);
    mkdir(TEST_ROOT, 0777);

    char dir_cusa[512], dir_ppsa[512];
    snprintf(dir_cusa, sizeof(dir_cusa), "%s/CUSA12345", TEST_ROOT);
    snprintf(dir_ppsa, sizeof(dir_ppsa), "%s/PPSA05432", TEST_ROOT);
    mkdir(dir_cusa, 0777);
    mkdir(dir_ppsa, 0777);

    // Create clips with known modification timestamps
    char f1[512], f2[512], f3[512], f4[512], f_ignored1[512], f_ignored2[512];
    snprintf(f1, sizeof(f1), "%s/clip_old.mp4", dir_cusa);
    snprintf(f2, sizeof(f2), "%s/clip_mid.webm", dir_cusa);
    snprintf(f3, sizeof(f3), "%s/clip_new.mp4", dir_cusa);
    snprintf(f4, sizeof(f4), "%s/game2.mp4", dir_ppsa);
    snprintf(f_ignored1, sizeof(f_ignored1), "%s/notes.txt", dir_cusa);
    snprintf(f_ignored2, sizeof(f_ignored2), "%s/.hidden.mp4", dir_cusa);

    create_file(f1, "old_clip_data", 13, 1000);
    create_file(f2, "mid_clip_data_webm", 18, 2000);
    create_file(f3, "new_clip_data_mp4", 17, 3000);
    create_file(f4, "ps5_game2_clip", 14, 2500);
    create_file(f_ignored1, "ignore me", 9, 2100);
    create_file(f_ignored2, "hidden", 6, 2900);

    capture_clip_t* clips = NULL;
    size_t count = 0;
    int rc = scan_existing_clips(TEST_ROOT, &clips, &count);
    assert(rc == 0);
    assert(count == 4);
    assert(clips != NULL);

    // Verify sorted descending by timestamp:
    // Expected order:
    // 0: f3 (3000) - CUSA12345
    // 1: f4 (2500) - PPSA05432
    // 2: f2 (2000) - CUSA12345
    // 3: f1 (1000) - CUSA12345
    assert(clips[0].timestamp == 3000);
    assert(strcmp(clips[0].filepath, f3) == 0);
    assert(strcmp(clips[0].title_id, "CUSA12345") == 0);
    assert(clips[0].size_bytes == 17);

    assert(clips[1].timestamp == 2500);
    assert(strcmp(clips[1].filepath, f4) == 0);
    assert(strcmp(clips[1].title_id, "PPSA05432") == 0);
    assert(clips[1].size_bytes == 14);

    assert(clips[2].timestamp == 2000);
    assert(strcmp(clips[2].filepath, f2) == 0);
    assert(strcmp(clips[2].title_id, "CUSA12345") == 0);
    assert(clips[2].size_bytes == 18);

    assert(clips[3].timestamp == 1000);
    assert(strcmp(clips[3].filepath, f1) == 0);
    assert(strcmp(clips[3].title_id, "CUSA12345") == 0);
    assert(clips[3].size_bytes == 13);

    free(clips);

    // Trailing slash test on directory with direct files
    char direct_root[512];
    snprintf(direct_root, sizeof(direct_root), "%s/CUSA12345/", TEST_ROOT);
    clips = NULL;
    count = 0;
    rc = scan_existing_clips(direct_root, &clips, &count);
    assert(rc == 0);
    assert(count == 3);
    assert(strcmp(clips[0].title_id, "CUSA12345") == 0);
    free(clips);

    // Empty folder test
    rm_rf(TEST_ROOT);
    mkdir(TEST_ROOT, 0777);
    clips = NULL;
    count = 99;
    rc = scan_existing_clips(TEST_ROOT, &clips, &count);
    assert(rc == 0);
    assert(count == 0);
    assert(clips == NULL);

    // Invalid parameters
    assert(scan_existing_clips(NULL, &clips, &count) == -1);
    assert(scan_existing_clips(TEST_ROOT, NULL, &count) == -1);
    assert(scan_existing_clips(TEST_ROOT, &clips, NULL) == -1);

    printf("  -> scan_existing_clips passed!\n");
}

static void test_write_debouncing_logic(void) {
    printf("[Test 2] Testing write debouncing logic (Review Focus #1)...\n");
    rm_rf(TEST_ROOT);
    rm_rf(TEST_CONFIG_DIR);
    mkdir(TEST_ROOT, 0777);
    config_init(TEST_CONFIG_DIR);

    char dir_cusa[512];
    snprintf(dir_cusa, sizeof(dir_cusa), "%s/CUSA00001", TEST_ROOT);
    mkdir(dir_cusa, 0777);

    test_watcher_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mutex, NULL);

    int rc = watcher_init(TEST_ROOT, test_clip_ready_cb, &ctx);
    assert(rc == 0);

    char test_clip[512];
    snprintf(test_clip, sizeof(test_clip), "%s/active_capture.mp4", dir_cusa);

    // 1. Initial creation with partial write (100 bytes)
    char chunk[100];
    memset(chunk, 'A', sizeof(chunk));
    create_file(test_clip, chunk, sizeof(chunk), 0);

    // First scan: watcher detects file for the first time. Not debounced yet.
    watcher_scan_once();
    assert(ctx.count == 0);

    // 2. File grows (now 200 bytes)
    append_file(test_clip, chunk, sizeof(chunk));
    watcher_scan_once();
    assert(ctx.count == 0);

    // 3. File grows again (now 300 bytes)
    append_file(test_clip, chunk, sizeof(chunk));
    watcher_scan_once();
    assert(ctx.count == 0);

    // 4. File stops growing (remains 300 bytes) across consecutive check!
    watcher_scan_once();
    assert(ctx.count == 1);
    assert(strcmp(ctx.clips[0].filepath, test_clip) == 0);
    assert(strcmp(ctx.clips[0].title_id, "CUSA00001") == 0);
    assert(ctx.clips[0].size_bytes == 300);

    // 5. Subsequent scan: must NOT re-trigger callback
    watcher_scan_once();
    assert(ctx.count == 1);

    // 6. Test zero-byte file handling: zero-byte file must NOT trigger callback
    char zero_file[512];
    snprintf(zero_file, sizeof(zero_file), "%s/zero.mp4", dir_cusa);
    create_file(zero_file, "", 0, 0);

    watcher_scan_once();
    assert(ctx.count == 1);
    watcher_scan_once();
    assert(ctx.count == 1);

    watcher_stop();
    pthread_mutex_destroy(&ctx.mutex);
    printf("  -> Write debouncing logic passed!\n");
}

static void test_deduplication_logic(void) {
    printf("[Test 3] Testing deduplication of already-uploaded clips (Review Focus #4)...\n");
    rm_rf(TEST_ROOT);
    rm_rf(TEST_CONFIG_DIR);
    mkdir(TEST_ROOT, 0777);
    config_init(TEST_CONFIG_DIR);

    char dir_cusa[512];
    snprintf(dir_cusa, sizeof(dir_cusa), "%s/CUSA99999", TEST_ROOT);
    mkdir(dir_cusa, 0777);

    char clip_uploaded[512], clip_new[512];
    snprintf(clip_uploaded, sizeof(clip_uploaded), "%s/clip_done.mp4", dir_cusa);
    snprintf(clip_new, sizeof(clip_new), "%s/clip_fresh.mp4", dir_cusa);

    create_file(clip_uploaded, "clip_done_data", 14, 0);
    create_file(clip_new, "clip_fresh_data", 15, 0);

    // Mark clip_uploaded as already uploaded in persistent history
    history_mark_uploaded(clip_uploaded, "mock_yt_vid_123");
    assert(history_is_uploaded(clip_uploaded) == 1);
    assert(history_is_uploaded(clip_new) == 0);

    test_watcher_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mutex, NULL);

    int rc = watcher_init(TEST_ROOT, test_clip_ready_cb, &ctx);
    assert(rc == 0);

    // Check 1: registers clip_new (clip_uploaded is skipped)
    watcher_scan_once();
    assert(ctx.count == 0);

    // Check 2: clip_new size stable
    watcher_scan_once();
    assert(ctx.count == 1);
    assert(strcmp(ctx.clips[0].filepath, clip_new) == 0);

    // Extra scan: nothing new
    watcher_scan_once();
    assert(ctx.count == 1);

    watcher_stop();
    pthread_mutex_destroy(&ctx.mutex);
    printf("  -> Deduplication logic passed!\n");
}

static void test_threaded_watcher_and_debouncing(void) {
    printf("[Test 4] Testing threaded watcher with real-time growing file...\n");
    rm_rf(TEST_ROOT);
    rm_rf(TEST_CONFIG_DIR);
    mkdir(TEST_ROOT, 0777);
    config_init(TEST_CONFIG_DIR);

    char dir_cusa[512];
    snprintf(dir_cusa, sizeof(dir_cusa), "%s/CUSA55555", TEST_ROOT);
    mkdir(dir_cusa, 0777);

    char live_clip[512];
    snprintf(live_clip, sizeof(live_clip), "%s/live_recording.mp4", dir_cusa);

    test_watcher_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mutex, NULL);

    watcher_set_poll_interval_ms(50);
    int rc = watcher_init(TEST_ROOT, test_clip_ready_cb, &ctx);
    assert(rc == 0);

    rc = watcher_start();
    assert(rc == 0);

    // Create file and simulate active recording over 300ms (writing 50 bytes every 40ms)
    create_file(live_clip, "init", 4, 0);
    for (int i = 0; i < 6; i++) {
        usleep(40 * 1000);
        append_file(live_clip, "12345678901234567890123456789012345678901234567890", 50);
        pthread_mutex_lock(&ctx.mutex);
        // During writes, file size keeps changing every ~40ms while poll interval is 50ms,
        // so it should not trigger ready callback
        assert(ctx.count == 0);
        pthread_mutex_unlock(&ctx.mutex);
    }

    // Now writing has stopped. Total size: 4 + (6 * 50) = 304 bytes.
    // Wait for at least 2 poll cycles (150ms > 2 * 50ms)
    usleep(160 * 1000);

    pthread_mutex_lock(&ctx.mutex);
    assert(ctx.count == 1);
    assert(strcmp(ctx.clips[0].filepath, live_clip) == 0);
    assert(ctx.clips[0].size_bytes == 304);
    pthread_mutex_unlock(&ctx.mutex);

    // Wait another 100ms and verify count stays 1
    usleep(100 * 1000);
    pthread_mutex_lock(&ctx.mutex);
    assert(ctx.count == 1);
    pthread_mutex_unlock(&ctx.mutex);

    // Clean stop
    watcher_stop();
    pthread_mutex_destroy(&ctx.mutex);
    printf("  -> Threaded watcher and debouncing passed!\n");
}

static void test_notifications_and_default_upload(void) {
    printf("[Test 5] Testing notification dispatch and auto-upload notifications...\n");
    notify_set_spy(test_notify_spy);

    pthread_mutex_lock(&s_notify_test_mutex);
    s_notification_count = 0;
    s_last_notification[0] = '\0';
    pthread_mutex_unlock(&s_notify_test_mutex);

    notify_send("Test Toast Message");
    pthread_mutex_lock(&s_notify_test_mutex);
    assert(s_notification_count == 1);
    assert(strcmp(s_last_notification, "Test Toast Message") == 0);
    pthread_mutex_unlock(&s_notify_test_mutex);

    // Test watcher_process_clip_upload notifications
    rm_rf(TEST_CONFIG_DIR);
    config_init(TEST_CONFIG_DIR);
    app_config_t cfg;
    config_load(&cfg);
    cfg.auto_upload = 1;
    snprintf(cfg.cookies, sizeof(cfg.cookies), "SAPISID=ValidSapisid123;");
    snprintf(cfg.session_token, sizeof(cfg.session_token), "ValidSessionToken");
    config_save(&cfg);

    capture_clip_t clip;
    snprintf(clip.filepath, sizeof(clip.filepath), "/tmp/nonexistent_clip.mp4");
    snprintf(clip.title_id, sizeof(clip.title_id), "CUSA11111");
    clip.size_bytes = 1024;
    clip.timestamp = time(NULL);

    // Point youtube endpoints to mock server port where nothing is listening (immediate failure)
    youtube_set_base_urls("http://127.0.0.1:49999/scotty", "http://127.0.0.1:49999/createvideo");

    int res = watcher_process_clip_upload(&clip);
    assert(res != 0); // Failed upload

    // Verify upload failed notification was dispatched
    pthread_mutex_lock(&s_notify_test_mutex);
    assert(strstr(s_last_notification, "YouTube: Upload failed") != NULL);
    pthread_mutex_unlock(&s_notify_test_mutex);

    notify_set_spy(NULL);
    printf("  -> Notification dispatch and auto-upload notifications passed!\n");
}

static void stop_invoking_cb(const capture_clip_t* clip, void* userdata) {
    (void)clip;
    int* called = (int*)userdata;
    *called = 1;
    // Calling watcher_stop from within the worker thread's callback must not deadlock
    watcher_stop();
}

static void test_stop_from_callback(void) {
    printf("[Test 6] Testing watcher_stop called from within callback (deadlock avoidance)...\n");
    rm_rf(TEST_ROOT);
    mkdir(TEST_ROOT, 0777);

    char dir_cusa[512];
    snprintf(dir_cusa, sizeof(dir_cusa), "%s/CUSA77777", TEST_ROOT);
    mkdir(dir_cusa, 0777);

    char clip[512];
    snprintf(clip, sizeof(clip), "%s/self_stop.mp4", dir_cusa);
    create_file(clip, "self_stop_data", 14, 0);

    int stopped_flag = 0;
    watcher_set_poll_interval_ms(30);
    watcher_init(TEST_ROOT, stop_invoking_cb, &stopped_flag);
    watcher_start();

    // Sleep enough for 2 scan cycles
    for (int i = 0; i < 20 && !stopped_flag; i++) {
        usleep(20 * 1000);
    }

    assert(stopped_flag == 1);
    // An external watcher_stop call afterwards should also be safe and no-op
    watcher_stop();
    printf("  -> watcher_stop from callback passed without deadlock!\n");
}

int main(void) {
    test_scan_existing_clips();
    test_write_debouncing_logic();
    test_deduplication_logic();
    test_threaded_watcher_and_debouncing();
    test_notifications_and_default_upload();
    test_stop_from_callback();

    // Clean up temporary directories
    rm_rf(TEST_ROOT);
    rm_rf(TEST_CONFIG_DIR);

    printf("Task 4 Watcher Tests Passed Successfully!\n");
    return 0;
}
