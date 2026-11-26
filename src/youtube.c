// YouTube Studio (WEB_CREATOR) bootstrap, Scotty resumable upload, and video creation/metadata client.
#include "app.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define STUDIO_ORIGIN "https://studio.youtube.com"
#define DEFAULT_CLIENT_VER "1.20261126.00.00"

static char g_studio_url[512];
static char g_scotty_url[512];
static char g_innertube_base[512];

static void ensure_urls(void) {
    if (!g_studio_url[0]) {
        const char *u = getenv("PS5YT_STUDIO_URL");
        snprintf(g_studio_url, sizeof g_studio_url, "%s", u && u[0] ? u : "https://studio.youtube.com/");
    }
    if (!g_scotty_url[0]) {
        const char *u = getenv("PS5YT_SCOTTY_URL");
        snprintf(g_scotty_url, sizeof g_scotty_url, "%s", u && u[0] ? u : "https://upload.youtube.com/upload/studio");
    }
    if (!g_innertube_base[0]) {
        const char *u = getenv("PS5YT_INNERTUBE_BASE");
        snprintf(g_innertube_base, sizeof g_innertube_base, "%s", u && u[0] ? u : "https://studio.youtube.com/youtubei/v1");
    }
}

void yt_init(void) {
    g_studio_url[0] = 0;
    g_scotty_url[0] = 0;
    g_innertube_base[0] = 0;
    ensure_urls();
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static size_t write_sbuf(char *p, size_t sz, size_t n, void *ud) {
    sbuf *b = ud;
    if (b->len + sz * n > 16u * 1024 * 1024) return 0;
    return sb_add(b, p, sz * n) ? 0 : sz * n;
}

static int build_auth_header(const char *jar_path, char *out, size_t len) {
    char sap[512] = "", p1[512] = "", p3[512] = "";
    int has_sap = (net_extract_cookie(jar_path, "SAPISID", sap, sizeof sap) == 0);
    int has_1p = (net_extract_cookie(jar_path, "__Secure-1PAPISID", p1, sizeof p1) == 0);
    int has_3p = (net_extract_cookie(jar_path, "__Secure-3PAPISID", p3, sizeof p3) == 0);
    if (!has_sap && !has_3p) {
        memset(sap, 0, sizeof sap);
        memset(p1, 0, sizeof p1);
        memset(p3, 0, sizeof p3);
        return -1;
    }

    long now = (long)time(NULL);
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "Authorization: ");
    int parts = 0;
    char h[128];

    const char *primary = has_sap ? sap : p3;
    if (sapisid_hash(primary, STUDIO_ORIGIN, now, h, sizeof h) == 0) {
        sb_printf(&b, "%sSAPISIDHASH %s", parts++ ? " " : "", h);
    }
    if (has_1p && sapisid_hash(p1, STUDIO_ORIGIN, now, h, sizeof h) == 0) {
        sb_printf(&b, "%sSAPISID1PHASH %s", parts++ ? " " : "", h);
    }
    if (has_3p && sapisid_hash(p3, STUDIO_ORIGIN, now, h, sizeof h) == 0) {
        sb_printf(&b, "%sSAPISID3PHASH %s", parts++ ? " " : "", h);
    }

    memset(sap, 0, sizeof sap);
    memset(p1, 0, sizeof p1);
    memset(p3, 0, sizeof p3);
    memset(h, 0, sizeof h);

    if (!parts || !b.data || b.len >= len) {
        sb_free(&b);
        return -1;
    }
    memcpy(out, b.data, b.len + 1);
    memset(b.data, 0, b.len);
    sb_free(&b);
    return 0;
}

static struct curl_slist *add_session_headers(struct curl_slist *hdr, const char *auth_hdr, const yt_session *s) {
    if (auth_hdr && auth_hdr[0]) hdr = curl_slist_append(hdr, auth_hdr);
    hdr = curl_slist_append(hdr, "Origin: " STUDIO_ORIGIN);
    hdr = curl_slist_append(hdr, "Referer: " STUDIO_ORIGIN "/");
    char idx_hdr[48];
    snprintf(idx_hdr, sizeof idx_hdr, "X-Goog-AuthUser: %s",
             s && s->session_index[0] ? s->session_index : "0");
    hdr = curl_slist_append(hdr, idx_hdr);
    if (s && s->delegated_session_id[0]) {
        char pid_hdr[128];
        snprintf(pid_hdr, sizeof pid_hdr, "X-Goog-PageId: %s", s->delegated_session_id);
        hdr = curl_slist_append(hdr, pid_hdr);
    }
    hdr = curl_slist_append(hdr, "Expect:");
    return hdr;
}

// Finds `"key": "value"` anywhere in an HTML/JSON payload and unescapes the JSON string.
static int extract_embedded_json_str(const char *haystack, const char *key, char *out, size_t len) {
    if (!haystack || !key || !out || !len) return -1;
    size_t kl = strlen(key);
    const char *p = haystack;
    while ((p = strstr(p, key)) != NULL) {
        if (p > haystack && p[-1] == '"' && p[kl] == '"') {
            int ok_prev = (p - 1 == haystack) || p[-2] == '{' || p[-2] == ',' ||
                          p[-2] == ' ' || p[-2] == '\t' || p[-2] == '\n' || p[-2] == '\r';
            if (ok_prev) {
                const char *q = p + kl + 1;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                if (*q == ':') {
                    q++;
                    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                    if (*q == '"' && json_string(q, out, len) == 0 && out[0]) return 0;
                }
            }
        }
        p += kl;
    }
    return -1;
}

static void copy_file_fallback(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return;
    }
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, r, out);
    fclose(in);
    fclose(out);
}

int yt_validate_and_bootstrap(const char *jar_path, int commit_jar, yt_session *sess, char *err, size_t errlen) {
    ensure_urls();
    if (!jar_path || !sess) return -1;

    char out_jar[640];
    snprintf(out_jar, sizeof out_jar, "%s.out", jar_path);

    char url[768];
    if (sess->channel_id[0] && !strstr(g_studio_url, "/channel/")) {
        size_t bl = strlen(g_studio_url);
        snprintf(url, sizeof url, "%s%schannel/%s", g_studio_url, (bl && g_studio_url[bl - 1] == '/') ? "" : "/",
                 sess->channel_id);
    } else {
        snprintf(url, sizeof url, "%s", g_studio_url);
    }
    if (sess->session_index[0] && strcmp(sess->session_index, "0") != 0) {
        size_t ul = strlen(url);
        snprintf(url + ul, sizeof url - ul, "%sauthuser=%s", strchr(url, '?') ? "&" : "?", sess->session_index);
    }

    CURL *c = curl_easy_init();
    if (!c) {
        if (err && errlen) snprintf(err, errlen, "curl init failed");
        return -1;
    }

    struct curl_slist *hdr = add_session_headers(NULL, NULL, sess);
    sbuf body;
    sb_init(&body);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, jar_path);
    curl_easy_setopt(c, CURLOPT_COOKIEJAR, out_jar);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    char *eff = NULL;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    } else if (err && errlen) {
        snprintf(err, errlen, "%s", curl_easy_strerror(rc));
    }
    int is_login_redirect = (eff && (strstr(eff, "accounts.google.com") != NULL || strstr(eff, "ServiceLogin") != NULL));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        sb_free(&body);
        unlink(out_jar);
        return -1;
    }
    if (is_login_redirect || status == 401 || status == 403) {
        if (err && errlen) snprintf(err, errlen, "YouTube cookies expired or signed out");
        sb_free(&body);
        unlink(out_jar);
        return 0;
    }
    if (status < 200 || status >= 400 || !body.data) {
        if (err && errlen) snprintf(err, errlen, "YouTube Studio answered HTTP %ld", status);
        sb_free(&body);
        unlink(out_jar);
        return -1;
    }

    char key[128] = "", ver[64] = "", cid[64] = "", del[64] = "", sidx[16] = "", cname[160] = "";
    extract_embedded_json_str(body.data, "INNERTUBE_API_KEY", key, sizeof key);
    extract_embedded_json_str(body.data, "INNERTUBE_CLIENT_VERSION", ver, sizeof ver);
    extract_embedded_json_str(body.data, "CHANNEL_ID", cid, sizeof cid);
    extract_embedded_json_str(body.data, "DELEGATED_SESSION_ID", del, sizeof del);
    extract_embedded_json_str(body.data, "SESSION_INDEX", sidx, sizeof sidx);
    if (extract_embedded_json_str(body.data, "CHANNEL_TITLE", cname, sizeof cname) != 0 &&
        extract_embedded_json_str(body.data, "channelTitle", cname, sizeof cname) != 0 &&
        extract_embedded_json_str(body.data, "channelName", cname, sizeof cname) != 0 &&
        extract_embedded_json_str(body.data, "accountName", cname, sizeof cname) != 0) {
        extract_embedded_json_str(body.data, "authorName", cname, sizeof cname);
    }
    sb_free(&body);

    if (key[0]) snprintf(sess->api_key, sizeof sess->api_key, "%s", key);
    if (ver[0]) snprintf(sess->client_version, sizeof sess->client_version, "%s", ver);
    else if (!sess->client_version[0]) snprintf(sess->client_version, sizeof sess->client_version, DEFAULT_CLIENT_VER);
    if (!sess->channel_id[0] && cid[0]) snprintf(sess->channel_id, sizeof sess->channel_id, "%s", cid);
    if (!sess->delegated_session_id[0] && del[0]) snprintf(sess->delegated_session_id, sizeof sess->delegated_session_id, "%s", del);
    if (!sess->session_index[0]) snprintf(sess->session_index, sizeof sess->session_index, "%s", sidx[0] ? sidx : "0");
    if (cname[0]) snprintf(sess->channel_name, sizeof sess->channel_name, "%s", cname);
    else if (!sess->channel_name[0]) snprintf(sess->channel_name, sizeof sess->channel_name, "YouTube Channel");

    if (!sess->api_key[0] || !sess->channel_id[0]) {
        if (err && errlen) snprintf(err, errlen, "could not verify YouTube Studio session (cookies may be expired)");
        unlink(out_jar);
        return 0;
    }

    if (commit_jar) {
        if (net_commit_jar(out_jar) != 0) {
            unlink(out_jar);
            if (err && errlen) snprintf(err, errlen, "could not save validated cookies");
            return -1;
        }
    } else {
        copy_file_fallback(out_jar, jar_path);
        unlink(out_jar);
    }
    return 1;
}

typedef struct {
    char upload_url[1024];
    char status[64];
    int64_t size_received;
    int has_size_received;
} scotty_hdrs;

static size_t capture_scotty_headers(char *p, size_t sz, size_t n, void *ud) {
    size_t len = sz * n;
    scotty_hdrs *sh = ud;
    char line[1200];
    size_t cpy = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, p, cpy);
    line[cpy] = 0;
    while (cpy && (line[cpy - 1] == '\r' || line[cpy - 1] == '\n')) line[--cpy] = 0;

    char *colon = strchr(line, ':');
    if (colon) {
        *colon = 0;
        char *val = str_trim(colon + 1);
        if (!strcasecmp(line, "x-goog-upload-url") || (!strcasecmp(line, "Location") && !sh->upload_url[0])) {
            snprintf(sh->upload_url, sizeof sh->upload_url, "%s", val);
        } else if (!strcasecmp(line, "x-goog-upload-status")) {
            snprintf(sh->status, sizeof sh->status, "%s", val);
        } else if (!strcasecmp(line, "x-goog-upload-size-received")) {
            sh->size_received = (int64_t)strtoll(val, NULL, 10);
            sh->has_size_received = 1;
        }
    }
    return len;
}

static int parse_scotty_id(const char *body, char *out, size_t len) {
    if (!body || !out || !len) return -1;
    out[0] = 0;
    const char *top = json_find(body, "scottyResourceId");
    if (top && *top == '"' && json_string(top, out, len) == 0 && out[0]) return 0;
    const char *res = json_find(body, "resourceId");
    if (res) {
        const char *sc = json_find(res, "scottyResourceId");
        if (sc) {
            const char *id = json_find(sc, "id");
            if (id && json_string(id, out, len) == 0 && out[0]) return 0;
        }
    }
    return extract_embedded_json_str(body, "scottyResourceId", out, len);
}

int yt_scotty_init(const yt_session *s, const char *frontend_id, int64_t file_size,
                   char *upload_url_out, size_t url_len, int *http_status, char *err, size_t errlen) {
    ensure_urls();
    if (http_status) *http_status = 0;
    if (!s || !frontend_id || !upload_url_out || !url_len) return -1;

    char snap[640];
    if (net_snapshot_jar(snap, sizeof snap) != 0) {
        if (err && errlen) snprintf(err, errlen, "not logged in");
        return -1;
    }
    char auth[512];
    if (build_auth_header(snap, auth, sizeof auth) != 0) {
        unlink(snap);
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        return -1;
    }

    char url[640];
    snprintf(url, sizeof url, "%s%sauthuser=%s", g_scotty_url, strchr(g_scotty_url, '?') ? "&" : "?",
             s->session_index[0] ? s->session_index : "0");

    sbuf jbody;
    sb_init(&jbody);
    sb_puts(&jbody, "{\"frontendUploadId\":");
    sb_json_str(&jbody, frontend_id);
    sb_puts(&jbody, "}");

    char len_hdr[64];
    snprintf(len_hdr, sizeof len_hdr, "x-goog-upload-header-content-length: %lld", (long long)file_size);

    struct curl_slist *hdr = add_session_headers(NULL, auth, s);
    memset(auth, 0, sizeof auth);
    hdr = curl_slist_append(hdr, "x-goog-upload-command: start");
    hdr = curl_slist_append(hdr, "x-goog-upload-protocol: resumable");
    hdr = curl_slist_append(hdr, len_hdr);
    hdr = curl_slist_append(hdr, "x-goog-upload-header-content-type: application/octet-stream");
    hdr = curl_slist_append(hdr, "Content-Type: application/json; charset=UTF-8");

    CURL *c = curl_easy_init();
    if (!c) {
        unlink(snap);
        curl_slist_free_all(hdr);
        sb_free(&jbody);
        return -1;
    }

    scotty_hdrs sh;
    memset(&sh, 0, sizeof sh);
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, snap);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, jbody.data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)jbody.len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, capture_scotty_headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &sh);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    unlink(snap);
    sb_free(&jbody);
    sb_free(&resp);

    if (http_status) *http_status = (int)status;
    if (rc != CURLE_OK) return -1;
    if (status == 401 || status == 403) {
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        return -1;
    }
    if ((status != 200 && status != 201) || !sh.upload_url[0]) {
        if (err && errlen) snprintf(err, errlen, "Scotty upload init failed (HTTP %ld)", status);
        return -1;
    }
    if (strlen(sh.upload_url) >= url_len) {
        if (err && errlen) snprintf(err, errlen, "Scotty upload URL too long");
        return -1;
    }
    snprintf(upload_url_out, url_len, "%s", sh.upload_url);
    return 0;
}

int yt_scotty_query(const yt_session *s, const char *upload_url, int64_t *offset_out, int *is_final_out,
                    char *scotty_id_out, size_t id_len, int *http_status, char *err, size_t errlen) {
    if (http_status) *http_status = 0;
    if (offset_out) *offset_out = 0;
    if (is_final_out) *is_final_out = 0;
    if (scotty_id_out && id_len) scotty_id_out[0] = 0;
    if (!s || !upload_url || !upload_url[0]) return -1;

    char snap[640];
    if (net_snapshot_jar(snap, sizeof snap) != 0) {
        if (err && errlen) snprintf(err, errlen, "not logged in");
        return -1;
    }
    char auth[512];
    if (build_auth_header(snap, auth, sizeof auth) != 0) {
        unlink(snap);
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        return -1;
    }

    struct curl_slist *hdr = add_session_headers(NULL, auth, s);
    memset(auth, 0, sizeof auth);
    hdr = curl_slist_append(hdr, "x-goog-upload-command: query");

    CURL *c = curl_easy_init();
    if (!c) {
        unlink(snap);
        curl_slist_free_all(hdr);
        return -1;
    }

    scotty_hdrs sh;
    memset(&sh, 0, sizeof sh);
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, upload_url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, snap);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, capture_scotty_headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &sh);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    unlink(snap);

    if (http_status) *http_status = (int)status;
    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }
    if (status == 401 || status == 403) {
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        sb_free(&resp);
        return -1;
    }
    if (status != 200 && status != 201) {
        if (err && errlen) snprintf(err, errlen, "Scotty status query failed (HTTP %ld)", status);
        sb_free(&resp);
        return -1;
    }

    int is_final = !strcasecmp(sh.status, "final");
    if (is_final_out) *is_final_out = is_final;
    if (offset_out && sh.has_size_received && sh.size_received >= 0) *offset_out = sh.size_received;
    if (is_final && scotty_id_out && id_len && resp.data) parse_scotty_id(resp.data, scotty_id_out, id_len);

    sb_free(&resp);
    return 0;
}

typedef struct {
    FILE *fp;
    int64_t remaining;
    yt_progress_fn cb;
    void *ud;
    volatile int *stop_flag;
} chunk_read_ctx;

static size_t read_chunk_cb(char *ptr, size_t size, size_t nitems, void *ud) {
    chunk_read_ctx *ctx = ud;
    if (ctx->stop_flag && *ctx->stop_flag) return CURL_READFUNC_ABORT;
    if (ctx->remaining <= 0) return 0;
    size_t want = size * nitems;
    if ((int64_t)want > ctx->remaining) want = (size_t)ctx->remaining;
    size_t got = fread(ptr, 1, want, ctx->fp);
    if (got > 0) {
        ctx->remaining -= (int64_t)got;
        if (ctx->cb) ctx->cb((int64_t)got, ctx->ud);
    }
    return got;
}

int yt_scotty_upload_chunk(const yt_session *s, const char *upload_url, FILE *fp, int64_t offset, int64_t chunk_len,
                           int is_final, yt_progress_fn cb, void *ud, volatile int *stop_flag,
                           char *scotty_id_out, size_t id_len, int *http_status, char *err, size_t errlen) {
    if (http_status) *http_status = 0;
    if (scotty_id_out && id_len) scotty_id_out[0] = 0;
    if (!s || !upload_url || !fp || offset < 0 || chunk_len < 0) return -1;

    if (chunk_len > 0 && fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
        if (err && errlen) snprintf(err, errlen, "cannot seek capture file");
        return -1;
    }

    char snap[640];
    if (net_snapshot_jar(snap, sizeof snap) != 0) {
        if (err && errlen) snprintf(err, errlen, "not logged in");
        return -1;
    }
    char auth[512];
    if (build_auth_header(snap, auth, sizeof auth) != 0) {
        unlink(snap);
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        return -1;
    }

    char off_hdr[64];
    snprintf(off_hdr, sizeof off_hdr, "x-goog-upload-offset: %lld", (long long)offset);
    struct curl_slist *hdr = add_session_headers(NULL, auth, s);
    memset(auth, 0, sizeof auth);
    hdr = curl_slist_append(hdr, is_final ? "x-goog-upload-command: upload, finalize" : "x-goog-upload-command: upload");
    hdr = curl_slist_append(hdr, off_hdr);
    hdr = curl_slist_append(hdr, "Content-Type: application/octet-stream");

    CURL *c = curl_easy_init();
    if (!c) {
        unlink(snap);
        curl_slist_free_all(hdr);
        return -1;
    }

    chunk_read_ctx ctx = {fp, chunk_len, cb, ud, stop_flag};
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, upload_url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, snap);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)chunk_len);
    if (chunk_len > 0) {
        curl_easy_setopt(c, CURLOPT_READFUNCTION, read_chunk_cb);
        curl_easy_setopt(c, CURLOPT_READDATA, &ctx);
    } else {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    }
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 45L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    unlink(snap);

    if (http_status) *http_status = (int)status;
    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }
    if (status == 401 || status == 403) {
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        sb_free(&resp);
        return -1;
    }
    if (status != 200 && status != 201) {
        if (err && errlen) snprintf(err, errlen, "Scotty upload failed (HTTP %ld)", status);
        sb_free(&resp);
        return -1;
    }
    if (is_final) {
        if (!resp.data || parse_scotty_id(resp.data, scotty_id_out, id_len) != 0) {
            if (err && errlen) snprintf(err, errlen, "missing scottyResourceId from YouTube response");
            sb_free(&resp);
            return -1;
        }
    }
    sb_free(&resp);
    return 0;
}

static void sb_append_context(sbuf *b, const yt_session *s) {
    sb_puts(b, "\"context\":{\"client\":{\"clientName\":\"WEB_CREATOR\",\"clientVersion\":");
    sb_json_str(b, s->client_version[0] ? s->client_version : DEFAULT_CLIENT_VER);
    sb_puts(b, ",\"hl\":\"en\",\"gl\":\"US\"}");
    if (s->delegated_session_id[0]) {
        sb_puts(b, ",\"user\":{\"onBehalfOfUser\":");
        sb_json_str(b, s->delegated_session_id);
        sb_puts(b, "}");
    }
    sb_puts(b, "}");
}

static int innertube_post(const yt_session *s, const char *endpoint, const char *json_body, size_t body_len,
                          sbuf *resp_out, int *http_status, char *err, size_t errlen) {
    ensure_urls();
    if (http_status) *http_status = 0;
    if (!s->api_key[0]) {
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "missing YouTube Studio API key");
        return -1;
    }
    char snap[640];
    if (net_snapshot_jar(snap, sizeof snap) != 0) {
        if (err && errlen) snprintf(err, errlen, "not logged in");
        return -1;
    }
    char auth[512];
    if (build_auth_header(snap, auth, sizeof auth) != 0) {
        unlink(snap);
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        return -1;
    }

    char ekey[256], url[1024];
    url_encode(s->api_key, ekey, sizeof ekey);
    snprintf(url, sizeof url, "%s/%s?key=%s&alt=json", g_innertube_base, endpoint, ekey);

    struct curl_slist *hdr = add_session_headers(NULL, auth, s);
    memset(auth, 0, sizeof auth);
    hdr = curl_slist_append(hdr, "Content-Type: application/json");

    CURL *c = curl_easy_init();
    if (!c) {
        unlink(snap);
        curl_slist_free_all(hdr);
        return -1;
    }

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, snap);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, resp_out);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    unlink(snap);

    if (http_status) *http_status = (int)status;
    if (rc != CURLE_OK) return -1;
    if (status == 401 || status == 403) {
        if (err && errlen) snprintf(err, errlen, "YouTube session expired — sign in again");
        return -1;
    }
    if (status != 200 && status != 201) {
        if (err && errlen) snprintf(err, errlen, "YouTube %s failed (HTTP %ld)", endpoint, status);
        return -1;
    }
    return 0;
}

int yt_create_video(const yt_session *s, const char *frontend_id, const char *scotty_id, const yt_video_meta *meta,
                    char *video_id_out, size_t vid_len, int *http_status, char *err, size_t errlen) {
    if (!s || !scotty_id || !scotty_id[0] || !meta || !video_id_out || !vid_len) return -1;
    video_id_out[0] = 0;

    const char *priv = meta->privacy[0] ? meta->privacy : "UNLISTED";
    const char *title = meta->title[0] ? meta->title : "PS5 Gameplay Capture";

    sbuf req, resp;
    sb_init(&req);
    sb_init(&resp);
    sb_puts(&req, "{");
    sb_append_context(&req, s);
    sb_puts(&req, ",\"channelId\":");
    sb_json_str(&req, s->channel_id);
    sb_puts(&req, ",\"resourceId\":{\"scottyResourceId\":{\"id\":");
    sb_json_str(&req, scotty_id);
    sb_puts(&req, "}}");
    if (frontend_id && frontend_id[0]) {
        sb_puts(&req, ",\"frontendUploadId\":");
        sb_json_str(&req, frontend_id);
    }
    sb_puts(&req, ",\"initialMetadata\":{\"title\":{\"newTitle\":");
    sb_json_str(&req, title);
    sb_puts(&req, "},\"description\":{\"newDescription\":");
    sb_json_str(&req, meta->description);
    sb_puts(&req, "},\"privacy\":{\"newPrivacy\":");
    sb_json_str(&req, priv);
    sb_puts(&req, "},\"draftState\":{\"isDraft\":false}}}");

    int rc = innertube_post(s, "upload/createvideo", req.data, req.len, &resp, http_status, err, errlen);
    sb_free(&req);
    if (rc == 0) {
        if (!resp.data || extract_embedded_json_str(resp.data, "videoId", video_id_out, vid_len) != 0) {
            if (err && errlen) snprintf(err, errlen, "missing videoId in createvideo response");
            rc = -1;
        }
    }
    sb_free(&resp);
    return rc;
}

int yt_update_metadata(const yt_session *s, const char *video_id, const yt_video_meta *meta,
                       int *http_status, char *err, size_t errlen) {
    if (!s || !video_id || !video_id[0] || !meta) return -1;

    sbuf req, resp;
    sb_init(&req);
    sb_init(&resp);
    sb_puts(&req, "{");
    sb_append_context(&req, s);
    sb_puts(&req, ",\"encryptedVideoId\":");
    sb_json_str(&req, video_id);
    sb_puts(&req, ",\"madeForKids\":{\"operation\":\"MDE_MADE_FOR_KIDS_UPDATE_OPERATION_SET\",\"newMfk\":");
    sb_puts(&req, meta->made_for_kids ? "\"MDE_MADE_FOR_KIDS_TYPE_MFK\"" : "\"MDE_MADE_FOR_KIDS_TYPE_NOT_MFK\"");
    sb_puts(&req, "}");
    if (!meta->made_for_kids && meta->playlist_id[0]) {
        sb_puts(&req, ",\"addToPlaylist\":{\"addToPlaylistIds\":[");
        sb_json_str(&req, meta->playlist_id);
        sb_puts(&req, "]}");
    }
    sb_puts(&req, "}");

    int rc = innertube_post(s, "video_manager/metadata_update", req.data, req.len, &resp, http_status, err, errlen);
    sb_free(&req);
    sb_free(&resp);
    return rc;
}
