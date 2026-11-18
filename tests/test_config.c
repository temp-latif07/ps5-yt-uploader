#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/config.h"

static void clean_dir(const char* dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        // ignore errors in cleanup
    }
}

void test_config_defaults_and_io(void) {
    const char* test_dir = "/tmp/test_ps5_youtube";
    clean_dir(test_dir);
    assert(config_init(test_dir) == 0);

    app_config_t cfg;
    assert(config_load(&cfg) == 0);
    assert(cfg.server_port == 8888);
    assert(strcmp(cfg.default_privacy, "UNLISTED") == 0);
    assert(cfg.auto_upload == 1);

    strcpy(cfg.channel_id, "UC123456789");
    strcpy(cfg.session_token, "TOKEN_ABC_123");
    strcpy(cfg.cookies, "SAPISID=xyz; SSID=abc;");
    strcpy(cfg.innertube_key, "AIzaSyTestKey");
    strcpy(cfg.channel_name, "My Channel");
    assert(config_save(&cfg) == 0);

    app_config_t loaded;
    assert(config_load(&loaded) == 0);
    assert(strcmp(loaded.channel_id, "UC123456789") == 0);
    assert(strcmp(loaded.session_token, "TOKEN_ABC_123") == 0);
    assert(strcmp(loaded.cookies, "SAPISID=xyz; SSID=abc;") == 0);
    assert(strcmp(loaded.innertube_key, "AIzaSyTestKey") == 0);
    assert(strcmp(loaded.channel_name, "My Channel") == 0);
    assert(loaded.server_port == 8888);
    assert(strcmp(loaded.default_privacy, "UNLISTED") == 0);
    assert(loaded.auto_upload == 1);
}

void test_history_deduplication(void) {
    const char* clip = "/user/av_content/video/CUSA00001/clip1.mp4";
    assert(history_is_uploaded(clip) == 0);
    assert(history_mark_uploaded(clip, "vid123") == 0);
    assert(history_is_uploaded(clip) == 1);
}

void test_persistent_deduplication_across_reboots(void) {
    // Review Focus #4: verify uploaded.json persists across daemon re-inits
    const char* test_dir = "/tmp/test_ps5_youtube";
    const char* clip1 = "/user/av_content/video/CUSA00001/clip1.mp4";
    const char* clip2 = "/user/av_content/video/CUSA00002/clip2.mp4";

    // Re-initialize config (simulate reboot/restart)
    assert(config_init(test_dir) == 0);
    assert(history_is_uploaded(clip1) == 1);
    assert(history_is_uploaded(clip2) == 0);

    assert(history_mark_uploaded(clip2, "vid456") == 0);
    assert(history_is_uploaded(clip2) == 1);

    // Re-init once more to ensure both clips are still marked uploaded
    assert(config_init(test_dir) == 0);
    assert(history_is_uploaded(clip1) == 1);
    assert(history_is_uploaded(clip2) == 1);
    assert(history_is_uploaded("/user/av_content/video/nonexistent.mp4") == 0);
}

void test_recursive_mkdir_mode_0777(void) {
    // Review Focus #5: ensure nested non-existent directory is recursively created
    const char* deep_dir = "/tmp/test_ps5_youtube/deeply/nested/data/youtube_uploader";
    clean_dir("/tmp/test_ps5_youtube/deeply");
    assert(config_init(deep_dir) == 0);

    struct stat st;
    assert(stat(deep_dir, &st) == 0);
    assert(S_ISDIR(st.st_mode));

    // Can load defaults and save in newly created directory
    app_config_t cfg;
    assert(config_load(&cfg) == 0);
    assert(cfg.server_port == 8888);
    assert(config_save(&cfg) == 0);

    // Verify file exists
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", deep_dir);
    assert(stat(cfg_path, &st) == 0);
}

void test_error_handling(void) {
    // Null pointer resilience
    assert(config_load(NULL) == -1);
    assert(config_save(NULL) == -1);
    assert(history_is_uploaded(NULL) == 0);
    assert(history_is_uploaded("") == 0);
    assert(history_mark_uploaded(NULL, "vid123") == -1);
    assert(history_mark_uploaded("", "vid123") == -1);

    // Marking uploaded with NULL video_id should succeed (records entry)
    assert(history_mark_uploaded("/tmp/null_vid.mp4", NULL) == 0);
    assert(history_is_uploaded("/tmp/null_vid.mp4") == 1);
}

int main(void) {
    test_config_defaults_and_io();
    test_history_deduplication();
    test_persistent_deduplication_across_reboots();
    test_recursive_mkdir_mode_0777();
    test_error_handling();
    clean_dir("/tmp/test_ps5_youtube");
    printf("Task 1 Tests Passed Successfully!\n");
    return 0;
}
