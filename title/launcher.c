// PS5 YouTube Uploader home-screen title (PPSA97631): starts the background payload and opens the browser.
//
// The server runs as an elfldr payload, not inside this title: a PS5 title closes when a game starts
// (and stops in rest mode), which would cut off long video uploads. The payload stays alive while you play.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelDebugOutText(int channel, const char *text);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *param);
int sceUserServiceInitialize(void *param);

#define SERVER_PORT 8888
#define ELFLDR_PORT 9021
#define PAYLOAD "/app0/ps5-yt-uploader.elf"

static void notify(const char *fmt, const char *arg) {
    notify_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, fmt, arg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    char line[3100];
    snprintf(line, sizeof line, "[ps5-yt-title] %s\n", req.message);
    sceKernelDebugOutText(0, line);
}

static int connect_local(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) {
        close(s);
        return -1;
    }
    return s;
}

static int server_up(void) {
    int s = connect_local(SERVER_PORT);
    if (s < 0) return 0;
    close(s);
    return 1;
}

static int send_payload(void) {
    int f = open(PAYLOAD, O_RDONLY);
    if (f < 0) return -2;
    int s = connect_local(ELFLDR_PORT);
    if (s < 0) {
        close(f);
        return -1;
    }
    static char buf[256 * 1024];
    ssize_t n;
    int rc = 0;
    while ((n = read(f, buf, sizeof buf)) > 0) {
        for (ssize_t at = 0; at < n;) {
            struct pollfd p = {s, POLLOUT, 0};
            if (poll(&p, 1, 10000) <= 0) { rc = -3; break; }
            ssize_t w = send(s, buf + at, n - at, 0);
            if (w <= 0) { rc = -3; break; }
            at += w;
        }
        if (rc) break;
    }
    close(f);
    close(s);
    return rc;
}

static void local_ip(char *out, size_t len) {
    snprintf(out, len, "<console IP>");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) {
        socklen_t l = sizeof a;
        if (!getsockname(s, (struct sockaddr *)&a, &l)) inet_ntop(AF_INET, &a.sin_addr, out, len);
    }
    close(s);
}

// app_crt.cpp calls ps5_fp_ieee() before _init_env() to set standard IEEE-754 MXCSR/x87 state.
__attribute__((weak, visibility("hidden"))) void ps5_fp_ieee(void) {
    const unsigned mxcsr = 0x1f80;
    const unsigned short cw = 0x37f;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

// A title must exit through the system service, never return normally or call exit().
void catchReturnFromMain(int status) {
    (void)status;
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
}

int main(void) {
    char ip[64], url[128];
    local_ip(ip, sizeof ip);
    snprintf(url, sizeof url, "http://%s:%d", ip, SERVER_PORT);
    int up = server_up();
    if (!up) {
        int rc = send_payload();
        if (rc == -1) notify("YouTube Uploader: elfldr is not listening on port 9021 (%s)", "no server started");
        else if (rc) notify("YouTube Uploader: could not start server (%s)", rc == -2 ? "ps5-yt-uploader.elf missing" : "send failed");
        else {
            for (int i = 0; i < 50 && !(up = server_up()); i++) usleep(200 * 1000);
            if (!up) notify("YouTube Uploader: server did not answer on %s yet", url);
        }
    }
    if (up) {
        sceUserServiceInitialize(NULL);
        if (sceSystemServiceLaunchWebBrowser("http://127.0.0.1:" "8888", NULL))
            notify("YouTube Uploader running: open %s from a phone or PC", url);
    }
    sleep(2);
    catchReturnFromMain(0);
    return 0;
}
