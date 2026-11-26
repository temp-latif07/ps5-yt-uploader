// Cookie jar management, session metadata persistence, and libcurl setup.
// Credentials are stored mode 0600 on disk and never logged or sent to the browser.
#include "app.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_cookie_path[600];
static char g_session_path[600];
static unsigned long g_tmp_seq;

void net_init(void) {
    snprintf(g_cookie_path, sizeof g_cookie_path, "%s/cookies.txt", plat_app_dir());
    snprintf(g_session_path, sizeof g_session_path, "%s/session.txt", plat_app_dir());
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

const char *net_cookie_path(void) {
    if (!g_cookie_path[0]) snprintf(g_cookie_path, sizeof g_cookie_path, "%s/cookies.txt", plat_app_dir());
    return g_cookie_path;
}

const char *net_session_path(void) {
    if (!g_session_path[0]) snprintf(g_session_path, sizeof g_session_path, "%s/session.txt", plat_app_dir());
    return g_session_path;
}

int net_has_cookies(void) {
    struct stat st;
    pthread_mutex_lock(&g_auth_lock);
    int ok = (!stat(net_cookie_path(), &st) && st.st_size > 0);
    pthread_mutex_unlock(&g_auth_lock);
    return ok;
}

static int host_mock_loopback(void) {
#ifdef __PROSPERO__
    return 0;
#else
    const char *u = getenv("PS5YT_STUDIO_URL");
    return u && (!strncmp(u, "http://127.0.0.1", 16) || !strncmp(u, "http://localhost", 16));
#endif
}

static int allowed_cookie_domain(const char *dom) {
    if (!dom || !dom[0]) return 0;
    const char *d = dom;
    if (!strncmp(d, "#HttpOnly_", 10)) d += 10;
    static const char *const suffixes[] = {
        "youtube.com", ".youtube.com",
        "google.com", ".google.com",
        "youtu.be", ".youtu.be",
    };
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        size_t dl = strlen(d), sl = strlen(suffixes[i]);
        if (dl == sl && !strcasecmp(d, suffixes[i])) return 1;
        if (dl > sl && suffixes[i][0] == '.' && !strcasecmp(d + dl - sl, suffixes[i])) return 1;
        if (dl > sl + 1 && suffixes[i][0] != '.' && d[dl - sl - 1] == '.' && !strcasecmp(d + dl - sl, suffixes[i])) return 1;
    }
#ifndef __PROSPERO__
    if (!strcasecmp(d, "127.0.0.1") || !strcasecmp(d, "localhost")) return 1;
#endif
    return 0;
}

static void emit_netscape_row(sbuf *out, const char *domain, const char *flag, const char *path,
                              const char *sec, const char *exp, const char *name, const char *val) {
    sb_printf(out, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", domain, flag, path, sec, exp, name, val);
}

static int parse_netscape_line(char *line, char *fields[7]) {
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p && n < 7) {
        if (*p == '\t') {
            *p = 0;
            fields[n++] = p + 1;
        }
        p++;
    }
    if (n != 7) return -1;
    // Reject additional tabs inside value
    if (strchr(fields[6], '\t')) return -1;
    return 0;
}

static int has_tab_row(const char *raw) {
    const char *p = raw;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && p[len - 1] == '\r') len--;
        if (len > 0 && (p[0] != '#' || !strncmp(p, "#HttpOnly_", 10))) {
            int tabs = 0;
            for (size_t i = 0; i < len; i++)
                if (p[i] == '\t') tabs++;
            if (tabs == 6) return 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

int net_normalize_cookies(const char *raw, sbuf *netscape_out, char *err, size_t errlen) {
    if (!raw || !netscape_out) {
        if (err && errlen) snprintf(err, errlen, "missing cookies");
        return -1;
    }
    sb_init(netscape_out);
    sb_puts(netscape_out, "# Netscape HTTP Cookie File\n");

    const char *src = raw;
    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
        src += 3;

    char *copy = strdup(src);
    if (!copy) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return -1;
    }

    int count = 0;
    int has_sapisid = 0;
    int loopback = host_mock_loopback();

    if (has_tab_row(copy)) {
        char *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            size_t len = strlen(line);
            while (len && line[len - 1] == '\r') line[--len] = 0;
            if (!len) continue;
            if (line[0] == '#' && strncmp(line, "#HttpOnly_", 10) != 0) continue;
            char *f[7];
            if (parse_netscape_line(line, f) != 0) continue;
            if (!f[5][0] || !allowed_cookie_domain(f[0])) continue;
            emit_netscape_row(netscape_out, f[0], f[1], f[2][0] ? f[2] : "/", f[3], f[4][0] ? f[4] : "0", f[5], f[6]);
            if (loopback && strncmp(f[0], "127.0.0.1", 9) != 0 && strncmp(f[0], "localhost", 9) != 0) {
                emit_netscape_row(netscape_out, "127.0.0.1", "FALSE", "/", "FALSE", "2147483647", f[5], f[6]);
            }
            if (!strcmp(f[5], "SAPISID") || !strcmp(f[5], "__Secure-3PAPISID")) has_sapisid = 1;
            count++;
        }
    } else {
        char *p = str_trim(copy);
        if (!strncasecmp(p, "Cookie:", 7)) p = str_trim(p + 7);
        char *save = NULL;
        for (char *tok = strtok_r(p, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            char *pair = str_trim(tok);
            if (!pair[0]) continue;
            char *eq = strchr(pair, '=');
            if (!eq || eq == pair) continue;
            *eq = 0;
            char *name = str_trim(pair);
            char *val = str_trim(eq + 1);
            if (!name[0]) continue;
            // Reject control chars or newlines in header values
            int bad = 0;
            for (const unsigned char *c = (const unsigned char *)name; *c; c++)
                if (*c < 0x21 || *c > 0x7e) bad = 1;
            for (const unsigned char *c = (const unsigned char *)val; *c; c++)
                if (*c < 0x20 || *c == 0x7f) bad = 1;
            if (bad) continue;
            emit_netscape_row(netscape_out, ".youtube.com", "TRUE", "/", "TRUE", "2147483647", name, val);
            emit_netscape_row(netscape_out, ".google.com", "TRUE", "/", "TRUE", "2147483647", name, val);
            if (loopback) {
                emit_netscape_row(netscape_out, "127.0.0.1", "FALSE", "/", "FALSE", "2147483647", name, val);
            }
            if (!strcmp(name, "SAPISID") || !strcmp(name, "__Secure-3PAPISID")) has_sapisid = 1;
            count++;
        }
    }

    memset(copy, 0, strlen(src));
    free(copy);

    if (count == 0 || !has_sapisid) {
        sb_free(netscape_out);
        if (err && errlen) {
            snprintf(err, errlen, count == 0 ? "no valid YouTube cookies found"
                                             : "missing SAPISID or __Secure-3PAPISID cookie (sign in to YouTube Studio first)");
        }
        return -1;
    }
    return 0;
}

int net_extract_cookie(const char *jar_path, const char *name, char *out, size_t len) {
    if (!jar_path || !name || !out || !len) return -1;
    out[0] = 0;
    FILE *f = fopen(jar_path, "r");
    if (!f) return -1;

    char line[8192];
    int found = 0;
    int best_yt = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        char *s = line;
        if (!strncmp(s, "#HttpOnly_", 10)) s += 10;
        else if (s[0] == '#') continue;

        char *fld[7];
        if (parse_netscape_line(s, fld) != 0) continue;
        if (strcmp(fld[5], name) != 0) continue;
        int is_yt = (strstr(fld[0], "youtube.com") != NULL);
        if (!found || (is_yt && !best_yt)) {
            snprintf(out, len, "%s", fld[6]);
            found = 1;
            best_yt = is_yt;
        }
    }
    memset(line, 0, sizeof line);
    fclose(f);
    return found && out[0] ? 0 : -1;
}

static void next_tmp_path(const char *prefix, char *out, size_t len) {
    pthread_mutex_lock(&g_auth_lock);
    unsigned long seq = ++g_tmp_seq;
    pthread_mutex_unlock(&g_auth_lock);
    snprintf(out, len, "%s/%s.%ld.%lu", plat_app_dir(), prefix, (long)getpid(), seq);
}

int net_write_temp_jar(const char *netscape_data, char *tmp_path_out, size_t len) {
    if (!netscape_data || !tmp_path_out || !len) return -1;
    next_tmp_path("cookies.tmp", tmp_path_out, len);
    int fd = open(tmp_path_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(netscape_data);
    size_t off = 0;
    int rc = 0;
    while (off < n) {
        ssize_t w = write(fd, netscape_data + off, n - off);
        if (w <= 0) { rc = -1; break; }
        off += (size_t)w;
    }
    close(fd);
    if (rc != 0) unlink(tmp_path_out);
    return rc;
}

int net_commit_jar(const char *tmp_path) {
    if (!tmp_path || !tmp_path[0]) return -1;
    char sap[256];
    if (net_extract_cookie(tmp_path, "SAPISID", sap, sizeof sap) != 0 &&
        net_extract_cookie(tmp_path, "__Secure-3PAPISID", sap, sizeof sap) != 0) {
        memset(sap, 0, sizeof sap);
        return -1;
    }
    memset(sap, 0, sizeof sap);

    pthread_mutex_lock(&g_auth_lock);
    chmod(tmp_path, 0600);
    int rc = rename(tmp_path, net_cookie_path());
    if (rc == 0) chmod(net_cookie_path(), 0600);
    pthread_mutex_unlock(&g_auth_lock);
    return rc;
}

int net_snapshot_jar(char *tmp_path_out, size_t len) {
    if (!tmp_path_out || !len) return -1;
    next_tmp_path("jar.snap", tmp_path_out, len);

    pthread_mutex_lock(&g_auth_lock);
    int in = open(net_cookie_path(), O_RDONLY);
    if (in < 0) {
        pthread_mutex_unlock(&g_auth_lock);
        tmp_path_out[0] = 0;
        return -1;
    }
    int out = open(tmp_path_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) {
        close(in);
        pthread_mutex_unlock(&g_auth_lock);
        tmp_path_out[0] = 0;
        return -1;
    }
    char buf[4096];
    ssize_t r;
    int rc = 0;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w <= 0) { rc = -1; break; }
            off += w;
        }
        if (rc) break;
    }
    if (r < 0) rc = -1;
    memset(buf, 0, sizeof buf);
    close(in);
    close(out);
    pthread_mutex_unlock(&g_auth_lock);
    if (rc != 0) {
        unlink(tmp_path_out);
        tmp_path_out[0] = 0;
    }
    return rc;
}

static void clean_val(char *s) {
    for (char *p = s; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
}

int net_session_get(yt_session *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->session_index, sizeof out->session_index, "0");

    pthread_mutex_lock(&g_auth_lock);
    FILE *f = fopen(net_session_path(), "r");
    if (!f) {
        pthread_mutex_unlock(&g_auth_lock);
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "channel_id")) snprintf(out->channel_id, sizeof out->channel_id, "%s", v);
        else if (!strcmp(k, "channel_name")) snprintf(out->channel_name, sizeof out->channel_name, "%s", v);
        else if (!strcmp(k, "delegated_session_id")) snprintf(out->delegated_session_id, sizeof out->delegated_session_id, "%s", v);
        else if (!strcmp(k, "session_index") && v[0]) snprintf(out->session_index, sizeof out->session_index, "%s", v);
        else if (!strcmp(k, "api_key")) snprintf(out->api_key, sizeof out->api_key, "%s", v);
        else if (!strcmp(k, "client_version")) snprintf(out->client_version, sizeof out->client_version, "%s", v);
    }
    fclose(f);
    pthread_mutex_unlock(&g_auth_lock);
    return 0;
}

int net_session_set(const yt_session *s) {
    if (!s) return -1;
    yt_session clean = *s;
    clean_val(clean.channel_id);
    clean_val(clean.channel_name);
    clean_val(clean.delegated_session_id);
    clean_val(clean.session_index);
    clean_val(clean.api_key);
    clean_val(clean.client_version);

    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", net_session_path());
    pthread_mutex_lock(&g_auth_lock);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        pthread_mutex_unlock(&g_auth_lock);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        pthread_mutex_unlock(&g_auth_lock);
        return -1;
    }
    fprintf(f,
            "channel_id=%s\nchannel_name=%s\ndelegated_session_id=%s\n"
            "session_index=%s\napi_key=%s\nclient_version=%s\n",
            clean.channel_id, clean.channel_name, clean.delegated_session_id,
            clean.session_index[0] ? clean.session_index : "0",
            clean.api_key, clean.client_version);
    fclose(f);
    chmod(tmp, 0600);
    int rc = rename(tmp, net_session_path());
    if (rc == 0) chmod(net_session_path(), 0600);
    else unlink(tmp);
    pthread_mutex_unlock(&g_auth_lock);
    return rc;
}

void net_auth_clear(void) {
    pthread_mutex_lock(&g_auth_lock);
    unlink(net_cookie_path());
    unlink(net_session_path());
    pthread_mutex_unlock(&g_auth_lock);
}

static int tune_sockbuf(void *ud, curl_socket_t fd, curlsocktype purpose) {
    (void)ud;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    int sizes[] = {4 << 20, 1 << 20, 512 << 10, 256 << 10};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int got = 0;
        socklen_t l = sizeof got;
        if (!setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sizes[i], sizeof sizes[i]) &&
            !getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &got, &l) && got >= (256 << 10))
            break;
    }
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int got = 0;
        socklen_t l = sizeof got;
        if (!setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sizes[i], sizeof sizes[i]) &&
            !getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &l) && got >= (256 << 10))
            break;
    }
    return CURL_SOCKOPT_OK;
}

void net_apply(void *h) {
    CURL *c = h;
    if (cacert_pem_len > 0) {
        struct curl_blob blob = {(void *)cacert_pem, cacert_pem_len, CURL_BLOB_NOCOPY};
        curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
    }
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, tune_sockbuf);
#ifdef __PROSPERO__
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
#endif
}
