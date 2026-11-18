#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "crypto.h"
#include "youtube.h"
#include "watcher.h"
#include "notify.h"
#include "server.h"
#include "api.h"
#include "media_tile.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#define DEFAULT_PORT 8888
#define DEFAULT_DATA_DIR "/data/youtube_uploader"
#define DEFAULT_CAPTURES_DIR "/user/av_content/video"
#define DEFAULT_APP_DB "/system_data/priv/mms/app.db"

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void on_clip_ready(const capture_clip_t* clip, void* userdata) {
    (void)userdata;
    if (clip) {
        watcher_process_clip_upload(clip);
    }
}

static void print_version(void) {
    printf("%s v%s (%s)\n", APP_NAME, APP_VERSION, APP_DISPLAY_NAME);
}

static void print_usage(const char* prog) {
    printf("%s v%s - %s\n", APP_NAME, APP_VERSION, APP_DISPLAY_NAME);
    printf("Usage: %s [options]\n", prog ? prog : APP_NAME);
    printf("Options:\n");
    printf("  -v, --version        Show version information\n");
    printf("  --port <p>           Port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  --data-dir <dir>     Persistent storage directory (default: %s)\n", DEFAULT_DATA_DIR);
    printf("  --captures-dir <dir> Video captures path (default: %s)\n", DEFAULT_CAPTURES_DIR);
    printf("  --app-db <path>      MMS database path (default: %s)\n", DEFAULT_APP_DB);
    printf("  --no-tile            Skip media tile registration\n");
    printf("  -h, --help           Show this help message\n");
}

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    int port_specified = 0;
    const char* data_dir = DEFAULT_DATA_DIR;
    const char* captures_dir = DEFAULT_CAPTURES_DIR;
    const char* app_db = DEFAULT_APP_DB;
    int no_tile = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            port_specified = 1;
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--captures-dir") == 0 && i + 1 < argc) {
            captures_dir = argv[++i];
        } else if (strcmp(argv[i], "--app-db") == 0 && i + 1 < argc) {
            app_db = argv[++i];
        } else if (strcmp(argv[i], "--no-tile") == 0) {
            no_tile = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number %d\n", port);
        return 1;
    }

    // Immediate startup notification for visual confirmation on PS5
    notify_send("YouTube Uploader starting...");

    // 1. config_init(data_dir): Ensure directory exists with 0777 and load config.json
    if (config_init(data_dir) != 0) {
        fprintf(stderr, "Error: Failed to initialize config in '%s'\n", data_dir);
        return 1;
    }

    app_config_t cfg;
    if (config_load(&cfg) != 0) {
        fprintf(stderr, "Warning: Failed to load config.json; using defaults\n");
    }
    if (port_specified) {
        cfg.server_port = port;
        config_save(&cfg);
    } else if (cfg.server_port > 0) {
        port = cfg.server_port;
    }

    // 2. media_tile_register(app_db, "YouTube Uploader", launch_url) (unless --no-tile)
    if (!no_tile) {
        char launch_url[128];
        snprintf(launch_url, sizeof(launch_url), "http://127.0.0.1:%d", port);
        int tile_rc = media_tile_register(app_db, "YouTube Uploader", launch_url);
        if (tile_rc != 0) {
            fprintf(stderr, "Warning: Failed to register media tile in MMS database '%s'\n", app_db ? app_db : "default");
        }
        media_tile_launch_browser(launch_url);
    }

    // 3. Signal handling (SIGINT, SIGTERM, SIGPIPE)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // 4. server_start(port)
    if (server_start(port) != 0) {
        fprintf(stderr, "Error: Failed to start HTTP server on port %d\n", port);
        return 1;
    }

    // 5. api_init(captures_dir)
    if (api_init(captures_dir) != 0) {
        fprintf(stderr, "Error: Failed to initialize REST API routes\n");
        server_stop();
        return 1;
    }

    // 6. watcher_init(captures_dir, on_clip_ready, NULL) and watcher_start()
    if (watcher_init(captures_dir, on_clip_ready, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize watcher for '%s'\n", captures_dir);
        api_cleanup();
        server_stop();
        return 1;
    }
    if (watcher_start() != 0) {
        fprintf(stderr, "Error: Failed to start watcher background thread\n");
        watcher_stop();
        api_cleanup();
        server_stop();
        return 1;
    }

    // 7. notify_send("YouTube Uploader running on port <port>")
    char notify_msg[128];
    snprintf(notify_msg, sizeof(notify_msg), "YouTube Uploader running on port %d", port);
    notify_send(notify_msg);

    while (g_running) {
        sleep(1);
    }

    printf("Shutting down YouTube Uploader gracefully...\n");
    watcher_stop();
    api_cleanup();
    server_stop();
    server_clear_routes();

    // Sync config
    app_config_t final_cfg;
    if (config_load(&final_cfg) == 0) {
        config_save(&final_cfg);
    }

    printf("YouTube Uploader stopped cleanly.\n");
    return 0;
}
