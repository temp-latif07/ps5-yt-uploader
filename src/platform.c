// Platform layer: PS5 payload (elfldr) and Mac/Linux host build for testing.
#include "app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelDebugOutText(int channel, const char *text);
#endif

static char g_app_dir[512];
static char g_captures_dir[512];
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void resolve_host_path(char *dst, size_t len, const char *val) {
    if (!val || !val[0]) {
        dst[0] = 0;
        return;
    }
    if (val[0] == '/') {
        snprintf(dst, len, "%s", val);
    } else {
        char cwd[384];
        if (getcwd(cwd, sizeof cwd)) {
            const char *rel = (val[0] == '.' && val[1] == '/') ? val + 2 : val;
            snprintf(dst, len, "%s/%s", cwd, rel);
        } else {
            snprintf(dst, len, "%s", val);
        }
    }
    size_t n = strlen(dst);
    while (n > 1 && dst[n - 1] == '/') dst[--n] = 0;
}

void plat_init(void) {
#ifdef __PROSPERO__
    snprintf(g_app_dir, sizeof g_app_dir, "/data/ps5-yt-uploader");
    snprintf(g_captures_dir, sizeof g_captures_dir, "/user/av_content/video");
#else
    const char *dir = getenv("PS5YT_APP_DIR");
    const char *cap = getenv("PS5YT_CAPTURES_DIR");
    resolve_host_path(g_app_dir, sizeof g_app_dir, dir && dir[0] ? dir : "./ps5yt-data");
    resolve_host_path(g_captures_dir, sizeof g_captures_dir, cap && cap[0] ? cap : "./ps5yt-data/captures");
#endif
    mkdir_p(g_app_dir);
}

const char *plat_app_dir(void) { return g_app_dir; }
const char *plat_captures_dir(void) { return g_captures_dir; }

const char *plat_mode(void) {
#if defined(PS5_TITLE)
    return "title";
#elif defined(__PROSPERO__)
    return "payload";
#else
    return "host";
#endif
}

void plat_log(const char *fmt, ...) {
    char line[1024];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d [ps5-yt] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
#ifdef __PROSPERO__
    sceKernelDebugOutText(0, line);
#else
    fputs(line, stderr);
#endif
    pthread_mutex_lock(&g_log_lock);
    if (g_app_dir[0]) {
        char path[600];
        snprintf(path, sizeof path, "%s/log.txt", g_app_dir);
        struct stat st;
        if (!stat(path, &st) && st.st_size > 512 * 1024) {
            char old[620];
            snprintf(old, sizeof old, "%s.1", path);
            rename(path, old);
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            if (write(fd, line, strlen(line)) < 0) { /* ignore */ }
            close(fd);
        }
    }
    pthread_mutex_unlock(&g_log_lock);
}

void plat_notify(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
#ifdef __PROSPERO__
    notify_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#endif
    plat_log("notify: %s", msg);
}

static int under(const char *path, const char *root) {
    if (!root || !root[0]) return 0;
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (n == 1 && root[0] == '/') return path[0] == '/';
    return !strncmp(path, root, n) && (path[n] == 0 || path[n] == '/');
}

int plat_path_allowed(const char *path) {
    if (!path || path[0] != '/' || strstr(path, "/../") || strstr(path, "/./")) return 0;
    size_t n = strlen(path);
    if ((n >= 3 && !strcmp(path + n - 3, "/..")) || (n >= 2 && !strcmp(path + n - 2, "/."))) return 0;
#ifdef __PROSPERO__
    if (under(path, "/user/av_content") || under(path, "/data")) return 1;
    if (!strncmp(path, "/mnt/usb", 8) || !strncmp(path, "/mnt/ext", 8)) {
        const char *p = path + 8;
        while (*p >= '0' && *p <= '9') p++;
        return p > path + 8 && (*p == 0 || *p == '/');
    }
    return 0;
#else
    if (g_captures_dir[0] && under(path, g_captures_dir)) return 1;
    const char *roots = getenv("PS5YT_ROOTS");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", roots && roots[0] ? roots : "/tmp");
    for (char *save = NULL, *r = strtok_r(buf, ":", &save); r; r = strtok_r(NULL, ":", &save))
        if (under(path, r)) return 1;
    return 0;
#endif
}

void plat_local_ip(char *out, size_t len) {
    snprintf(out, len, "127.0.0.1");
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

int plat_start_thread(void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc) plat_log("pthread_create failed: %d", rc);
    return rc;
}

int64_t plat_now_ms(void) {
    struct timeval tv; // clock_gettime is not exported to titles
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void plat_sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
