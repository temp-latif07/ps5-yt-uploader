#define _POSIX_C_SOURCE 200809L

#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/wait.h>

static int run_command(const char* cmd, char* out_buf, size_t max_out) {
    if (out_buf && max_out > 0) {
        out_buf[0] = '\0';
    }
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    size_t total_read = 0;
    if (out_buf && max_out > 1) {
        while (fgets(out_buf + total_read, (int)(max_out - total_read), fp) != NULL) {
            total_read = strlen(out_buf);
            if (total_read >= max_out - 1) break;
        }
    }
    int status = pclose(fp);
    if (status == -1) return -1;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static void test_version_constants(void) {
    printf("[Test 1] Verifying src/version.h constants...\n");
    assert(strlen(APP_VERSION) > 0);
    assert(strcmp(APP_NAME, "ps5-yt-uploader") == 0);
    assert(strcmp(APP_DISPLAY_NAME, "PS5 YouTube Uploader") == 0);
    printf("  -> APP_VERSION: %s\n", APP_VERSION);
    printf("  -> APP_NAME: %s\n", APP_NAME);
    printf("  -> APP_DISPLAY_NAME: %s\n", APP_DISPLAY_NAME);
}

static void test_cli_long_version(void) {
    printf("[Test 2] Testing CLI flag --version...\n");
    char output[512] = {0};
    int exit_code = run_command("./bin/ps5-youtube-host --version 2>&1", output, sizeof(output));
    printf("  -> Exit code: %d, output: %s", exit_code, output);
    assert(exit_code == 0);
    char expected[256];
    snprintf(expected, sizeof(expected), "%s v%s", APP_NAME, APP_VERSION);
    assert(strstr(output, expected) != NULL);
}

static void test_cli_short_version(void) {
    printf("[Test 3] Testing CLI flag -v...\n");
    char output[512] = {0};
    int exit_code = run_command("./bin/ps5-youtube-host -v 2>&1", output, sizeof(output));
    printf("  -> Exit code: %d, output: %s", exit_code, output);
    assert(exit_code == 0);
    char expected[256];
    snprintf(expected, sizeof(expected), "%s v%s", APP_NAME, APP_VERSION);
    assert(strstr(output, expected) != NULL);
}

int main(void) {
    printf("=== Running Version & CLI Tests ===\n");
    test_version_constants();
    test_cli_long_version();
    test_cli_short_version();
    printf("All Version & CLI Tests Passed Successfully!\n");
    return 0;
}
