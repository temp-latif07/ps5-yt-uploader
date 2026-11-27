// OAuth credential persistence (mode 0600), in-memory access token cache, and libcurl setup.
// Secrets and tokens are never logged or sent to the browser.
#include "app.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_oauth_path[600];
static char g_access_token[2048];
static int64_t g_token_expires_ms;

void net_init(void) {
    pthread_mutex_lock(&g_auth_lock);
    snprintf(g_oauth_path, sizeof g_oauth_path, "%s/oauth.txt", plat_app_dir());
    memset(g_access_token, 0, sizeof g_access_token);
    g_token_expires_ms = 0;
    pthread_mutex_unlock(&g_auth_lock);
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

const char *net_oauth_path(void) {
    pthread_mutex_lock(&g_auth_lock);
    if (!g_oauth_path[0]) snprintf(g_oauth_path, sizeof g_oauth_path, "%s/oauth.txt", plat_app_dir());
    pthread_mutex_unlock(&g_auth_lock);
    return g_oauth_path;
}

static void clean_val(char *s) {
    for (char *p = s; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
}

static int net_oauth_get_locked(yt_oauth *out) {
    memset(out, 0, sizeof *out);
    if (!g_oauth_path[0]) snprintf(g_oauth_path, sizeof g_oauth_path, "%s/oauth.txt", plat_app_dir());
    FILE *f = fopen(g_oauth_path, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "client_id")) snprintf(out->client_id, sizeof out->client_id, "%s", v);
        else if (!strcmp(k, "client_secret")) snprintf(out->client_secret, sizeof out->client_secret, "%s", v);
        else if (!strcmp(k, "refresh_token")) snprintf(out->refresh_token, sizeof out->refresh_token, "%s", v);
        else if (!strcmp(k, "channel_id")) snprintf(out->channel_id, sizeof out->channel_id, "%s", v);
        else if (!strcmp(k, "channel_name")) snprintf(out->channel_name, sizeof out->channel_name, "%s", v);
    }
    memset(line, 0, sizeof line);
    fclose(f);
    return 0;
}

static int net_oauth_set_locked(const yt_oauth *o) {
    yt_oauth clean = *o;
    clean_val(clean.client_id);
    clean_val(clean.client_secret);
    clean_val(clean.refresh_token);
    clean_val(clean.channel_id);
    clean_val(clean.channel_name);

    if (!g_oauth_path[0]) snprintf(g_oauth_path, sizeof g_oauth_path, "%s/oauth.txt", plat_app_dir());
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_oauth_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        memset(&clean, 0, sizeof clean);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        memset(&clean, 0, sizeof clean);
        return -1;
    }
    fprintf(f,
            "client_id=%s\nclient_secret=%s\nrefresh_token=%s\n"
            "channel_id=%s\nchannel_name=%s\n",
            clean.client_id, clean.client_secret, clean.refresh_token,
            clean.channel_id, clean.channel_name);
    int ferr = ferror(f);
    fclose(f);
    memset(&clean, 0, sizeof clean);
    if (ferr) {
        unlink(tmp);
        return -1;
    }
    chmod(tmp, 0600);
    int rc = rename(tmp, g_oauth_path);
    if (rc == 0) chmod(g_oauth_path, 0600);
    else unlink(tmp);
    return rc;
}

int net_oauth_get(yt_oauth *out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_auth_lock);
    int rc = net_oauth_get_locked(out);
    pthread_mutex_unlock(&g_auth_lock);
    return rc;
}

int net_oauth_set(const yt_oauth *o) {
    if (!o) return -1;
    pthread_mutex_lock(&g_auth_lock);
    int rc = net_oauth_set_locked(o);
    pthread_mutex_unlock(&g_auth_lock);
    return rc;
}

int net_oauth_has_client(void) {
    yt_oauth o;
    int ok = (net_oauth_get(&o) == 0 && o.client_id[0] && o.client_secret[0]);
    memset(&o, 0, sizeof o);
    return ok;
}

int net_oauth_is_logged_in(void) {
    yt_oauth o;
    int ok = (net_oauth_get(&o) == 0 && o.client_id[0] && o.client_secret[0] && o.refresh_token[0]);
    memset(&o, 0, sizeof o);
    return ok;
}

void net_oauth_clear_tokens(void) {
    pthread_mutex_lock(&g_auth_lock);
    memset(g_access_token, 0, sizeof g_access_token);
    g_token_expires_ms = 0;
    yt_oauth o;
    if (net_oauth_get_locked(&o) == 0 && o.client_id[0] && o.client_secret[0]) {
        memset(o.refresh_token, 0, sizeof o.refresh_token);
        memset(o.channel_id, 0, sizeof o.channel_id);
        memset(o.channel_name, 0, sizeof o.channel_name);
        net_oauth_set_locked(&o);
    }
    memset(&o, 0, sizeof o);
    pthread_mutex_unlock(&g_auth_lock);
}

void net_oauth_clear_all(void) {
    pthread_mutex_lock(&g_auth_lock);
    memset(g_access_token, 0, sizeof g_access_token);
    g_token_expires_ms = 0;
    if (!g_oauth_path[0]) snprintf(g_oauth_path, sizeof g_oauth_path, "%s/oauth.txt", plat_app_dir());
    unlink(g_oauth_path);
    pthread_mutex_unlock(&g_auth_lock);
}

static int has_ctl(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 1;
    return 0;
}

int net_parse_client_json(const char *json, char *cid, size_t cid_len,
                          char *csec, size_t csec_len, char *err, size_t errlen) {
    if (cid && cid_len) cid[0] = 0;
    if (csec && csec_len) csec[0] = 0;
    if (!json || !cid || !cid_len || !csec || !csec_len) {
        if (err && errlen) snprintf(err, errlen, "missing OAuth client JSON");
        return -1;
    }

    const char *p = json;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    const char *obj = json_find(p, "installed");
    if (!obj) obj = json_find(p, "web");
    if (!obj) obj = p;

    const char *v_id = json_find(obj, "client_id");
    const char *v_sec = json_find(obj, "client_secret");
    if (!v_id || !v_sec || json_string(v_id, cid, cid_len) != 0 || json_string(v_sec, csec, csec_len) != 0) {
        cid[0] = 0;
        csec[0] = 0;
        if (err && errlen)
            snprintf(err, errlen, "JSON is missing client_id or client_secret — download the OAuth client JSON from Google Cloud Console");
        return -1;
    }
    str_trim(cid);
    str_trim(csec);
    if (!cid[0] || !csec[0] || has_ctl(cid) || has_ctl(csec)) {
        cid[0] = 0;
        csec[0] = 0;
        if (err && errlen)
            snprintf(err, errlen, "JSON is missing client_id or client_secret — download the OAuth client JSON from Google Cloud Console");
        return -1;
    }
    return 0;
}

int net_token_cache_get(char *out, size_t len) {
    if (!out || !len) return -1;
    out[0] = 0;
    pthread_mutex_lock(&g_auth_lock);
    int ok = (g_access_token[0] && plat_now_ms() < g_token_expires_ms);
    if (ok) snprintf(out, len, "%s", g_access_token);
    pthread_mutex_unlock(&g_auth_lock);
    return ok ? 0 : -1;
}

void net_token_cache_set(const char *token, int expires_in_s) {
    pthread_mutex_lock(&g_auth_lock);
    if (!token || !token[0] || expires_in_s <= 60) {
        memset(g_access_token, 0, sizeof g_access_token);
        g_token_expires_ms = 0;
    } else {
        snprintf(g_access_token, sizeof g_access_token, "%s", token);
        g_token_expires_ms = plat_now_ms() + (int64_t)(expires_in_s - 60) * 1000;
    }
    pthread_mutex_unlock(&g_auth_lock);
}

void net_token_cache_clear(void) {
    pthread_mutex_lock(&g_auth_lock);
    memset(g_access_token, 0, sizeof g_access_token);
    g_token_expires_ms = 0;
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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ps5-yt-uploader/" APP_VERSION);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, tune_sockbuf);
#ifdef __PROSPERO__
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
#endif
}
