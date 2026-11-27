// PS5 YouTube Uploader: background HTTP server and resumable YouTube uploader payload.
#include "app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_sig(int sig) {
    (void)sig;
    httpd_stop();
}

int main(int argc, char **argv) {
#ifndef __PROSPERO__
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("%s %s\n", APP_NAME, APP_VERSION);
            return 0;
        }
    }
#else
    (void)argc;
    (void)argv;
#endif

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    plat_init();
    net_init();
    yt_init();
    clips_init();
    plat_log("PS5 YouTube Uploader %s (%s) starting, data in %s", APP_VERSION, plat_mode(), plat_app_dir());
    upload_init();

    int port = APP_PORT;
    const char *env = getenv("PS5YT_PORT");
    if (env && atoi(env) > 0) port = atoi(env);

    plat_register_shell_tile(port);

    char ip[64];
    plat_local_ip(ip, sizeof ip);
    plat_notify("YouTube Uploader: open http://%s:%d in a browser", ip, port);
    if (httpd_run(port) < 0) {
        plat_notify("YouTube Uploader: port %d is busy (already running?)", port);
        upload_shutdown();
        return 1;
    }

    upload_shutdown();
    plat_notify("YouTube Uploader: server stopped");
    return 0;
}
