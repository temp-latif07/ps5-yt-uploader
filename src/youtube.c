// Google OAuth 2.0 TV Device Flow and YouTube Data API v3 resumable upload client.
#include "app.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define YT_SCOPE "https://www.googleapis.com/auth/youtube"

static char g_oauth_base[512];
static char g_api_base[512];
static char g_upload_base[512];

static void set_base_url(char *dst, size_t len, const char *env_name, const char *def_url) {
    const char *u = getenv(env_name);
    snprintf(dst, len, "%s", (u && u[0]) ? u : def_url);
    size_t n = strlen(dst);
    while (n > 1 && dst[n - 1] == '/') dst[--n] = 0;
}

static void ensure_urls(void) {
    if (!g_oauth_base[0]) set_base_url(g_oauth_base, sizeof g_oauth_base, "PS5YT_OAUTH_BASE", "https://oauth2.googleapis.com");
    if (!g_api_base[0]) set_base_url(g_api_base, sizeof g_api_base, "PS5YT_API_BASE", "https://www.googleapis.com/youtube/v3");
    if (!g_upload_base[0]) set_base_url(g_upload_base, sizeof g_upload_base, "PS5YT_UPLOAD_BASE", "https://www.googleapis.com/upload/youtube/v3");
}

void yt_init(void) {
    g_oauth_base[0] = 0;
    g_api_base[0] = 0;
    g_upload_base[0] = 0;
    ensure_urls();
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static size_t write_sbuf(char *p, size_t sz, size_t n, void *ud) {
    sbuf *b = ud;
    if (b->len + sz * n > 16u * 1024 * 1024) return 0;
    return sb_add(b, p, sz * n) ? 0 : sz * n;
}

static struct curl_slist *append_bearer(struct curl_slist *hdr, const char *access_token) {
    char auth[2200];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", access_token ? access_token : "");
    hdr = curl_slist_append(hdr, auth);
    memset(auth, 0, sizeof auth);
    hdr = curl_slist_append(hdr, "Expect:");
    return hdr;
}

static void extract_oauth_err(const char *body, char *err_code, size_t code_len,
                              char *desc, size_t desc_len) {
    if (err_code && code_len) err_code[0] = 0;
    if (desc && desc_len) desc[0] = 0;
    if (!body) return;
    const char *ev = json_find(body, "error");
    if (ev && *ev == '"' && err_code && code_len) json_string(ev, err_code, code_len);
    const char *dv = json_find(body, "error_description");
    if (dv && *dv == '"' && desc && desc_len) json_string(dv, desc, desc_len);
}

static int response_has_quota_err(const char *body) {
    if (!body) return 0;
    return strstr(body, "quotaExceeded") != NULL || strstr(body, "uploadLimitExceeded") != NULL;
}

typedef struct {
    char location[1024];
    int64_t range_end;
    int has_range;
} upload_hdrs;

static size_t capture_upload_headers(char *p, size_t sz, size_t n, void *ud) {
    size_t len = sz * n;
    upload_hdrs *uh = ud;
    char line[1200];
    size_t cpy = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, p, cpy);
    line[cpy] = 0;
    while (cpy && (line[cpy - 1] == '\r' || line[cpy - 1] == '\n')) line[--cpy] = 0;

    char *colon = strchr(line, ':');
    if (colon) {
        *colon = 0;
        char *val = str_trim(colon + 1);
        if (!strcasecmp(line, "Location")) {
            snprintf(uh->location, sizeof uh->location, "%s", val);
        } else if (!strcasecmp(line, "Range")) {
            char *dash = strchr(val, '-');
            if (dash && dash[1] >= '0' && dash[1] <= '9') {
                uh->range_end = (int64_t)strtoll(dash + 1, NULL, 10);
                uh->has_range = 1;
            }
        }
    }
    return len;
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

int yt_oauth_start_device(yt_device_flow *out, char *err, size_t errlen) {
    ensure_urls();
    if (!out) return -1;
    memset(out, 0, sizeof *out);

    yt_oauth o;
    if (net_oauth_get(&o) != 0 || !o.client_id[0] || !o.client_secret[0]) {
        memset(&o, 0, sizeof o);
        if (err && errlen) snprintf(err, errlen, "OAuth Client ID is not configured");
        return -1;
    }

    char ecid[1024], escope[256], url[640];
    url_encode(o.client_id, ecid, sizeof ecid);
    url_encode(YT_SCOPE, escope, sizeof escope);
    memset(&o, 0, sizeof o);
    snprintf(url, sizeof url, "%s/device/code", g_oauth_base);

    sbuf form, resp;
    sb_init(&form);
    sb_init(&resp);
    sb_printf(&form, "client_id=%s&scope=%s", ecid, escope);

    CURL *c = curl_easy_init();
    if (!c) {
        sb_free(&form);
        sb_free(&resp);
        if (err && errlen) snprintf(err, errlen, "curl init failed");
        return -1;
    }

    struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
    hdr = curl_slist_append(hdr, "Expect:");

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)form.len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    sb_free(&form);

    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }

    if (status != 200 || !resp.data) {
        char ecode[64] = "", edesc[200] = "";
        extract_oauth_err(resp.data, ecode, sizeof ecode, edesc, sizeof edesc);
        if (err && errlen) {
            if (edesc[0]) snprintf(err, errlen, "%s (make sure OAuth client type is TVs and Limited Input devices)", edesc);
            else if (ecode[0]) snprintf(err, errlen, "Google OAuth error: %s (use TVs and Limited Input devices client type)", ecode);
            else snprintf(err, errlen, "Google device code request failed (HTTP %ld)", status);
        }
        sb_free(&resp);
        return -1;
    }

    const char *v_dc = json_find(resp.data, "device_code");
    const char *v_uc = json_find(resp.data, "user_code");
    const char *v_vu = json_find(resp.data, "verification_url");
    if (!v_vu) v_vu = json_find(resp.data, "verification_uri");
    const char *v_exp = json_find(resp.data, "expires_in");
    const char *v_int = json_find(resp.data, "interval");

    if (v_dc) json_string(v_dc, out->device_code, sizeof out->device_code);
    if (v_uc) json_string(v_uc, out->user_code, sizeof out->user_code);
    if (v_vu) json_string(v_vu, out->verification_url, sizeof out->verification_url);
    out->expires_in = (int)json_int(v_exp, 1800);
    out->interval_s = (int)json_int(v_int, 5);
    if (out->interval_s < 1) out->interval_s = 1;
    if (out->expires_in < 30) out->expires_in = 1800;
    out->expires_at_ms = plat_now_ms() + (int64_t)out->expires_in * 1000;

    if (resp.data) memset(resp.data, 0, resp.len);
    sb_free(&resp);

    if (!out->device_code[0] || !out->user_code[0] || !out->verification_url[0]) {
        memset(out, 0, sizeof *out);
        if (err && errlen) snprintf(err, errlen, "incomplete device code response from Google");
        return -1;
    }
    return 0;
}

int yt_oauth_poll_device(yt_device_flow *flow, char *state_out, size_t state_len,
                         char *err, size_t errlen) {
    ensure_urls();
    if (state_out && state_len) snprintf(state_out, state_len, "error");
    if (!flow || !flow->device_code[0]) {
        if (err && errlen) snprintf(err, errlen, "no active device sign-in code");
        return -1;
    }

    yt_oauth o;
    if (net_oauth_get(&o) != 0 || !o.client_id[0] || !o.client_secret[0]) {
        memset(&o, 0, sizeof o);
        if (err && errlen) snprintf(err, errlen, "OAuth Client ID is not configured");
        return -1;
    }

    char ecid[1024], esec[1024], edev[2048], url[640];
    url_encode(o.client_id, ecid, sizeof ecid);
    url_encode(o.client_secret, esec, sizeof esec);
    url_encode(flow->device_code, edev, sizeof edev);
    snprintf(url, sizeof url, "%s/token", g_oauth_base);

    sbuf form, resp;
    sb_init(&form);
    sb_init(&resp);
    sb_printf(&form,
              "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code"
              "&client_id=%s&client_secret=%s&device_code=%s",
              ecid, esec, edev);
    memset(esec, 0, sizeof esec);
    memset(edev, 0, sizeof edev);

    CURL *c = curl_easy_init();
    if (!c) {
        memset(&o, 0, sizeof o);
        if (form.data) memset(form.data, 0, form.len);
        sb_free(&form);
        sb_free(&resp);
        if (err && errlen) snprintf(err, errlen, "curl init failed");
        return -1;
    }

    struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
    hdr = curl_slist_append(hdr, "Expect:");

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)form.len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (form.data) memset(form.data, 0, form.len);
    sb_free(&form);

    if (rc != CURLE_OK) {
        memset(&o, 0, sizeof o);
        sb_free(&resp);
        return -1;
    }

    if (status == 200 && resp.data) {
        char access_token[2048] = "";
        char refresh_token[512] = "";
        const char *v_at = json_find(resp.data, "access_token");
        const char *v_rt = json_find(resp.data, "refresh_token");
        int exp = (int)json_int(json_find(resp.data, "expires_in"), 3600);
        if (v_at) json_string(v_at, access_token, sizeof access_token);
        if (v_rt) json_string(v_rt, refresh_token, sizeof refresh_token);
        memset(resp.data, 0, resp.len);
        sb_free(&resp);

        if (!access_token[0] || !refresh_token[0]) {
            memset(access_token, 0, sizeof access_token);
            memset(refresh_token, 0, sizeof refresh_token);
            memset(&o, 0, sizeof o);
            if (err && errlen) snprintf(err, errlen, "Google did not return an OAuth refresh token — revoke app access and retry");
            return -1;
        }

        int ch_http = 0;
        char cid[64] = "", cname[160] = "";
        if (yt_fetch_channel(access_token, cid, sizeof cid, cname, sizeof cname, &ch_http, err, errlen) != 0) {
            memset(access_token, 0, sizeof access_token);
            memset(refresh_token, 0, sizeof refresh_token);
            memset(&o, 0, sizeof o);
            return -1;
        }

        snprintf(o.refresh_token, sizeof o.refresh_token, "%s", refresh_token);
        snprintf(o.channel_id, sizeof o.channel_id, "%s", cid);
        snprintf(o.channel_name, sizeof o.channel_name, "%s", cname);
        int save_rc = net_oauth_set(&o);
        memset(&o, 0, sizeof o);
        memset(refresh_token, 0, sizeof refresh_token);

        if (save_rc != 0) {
            memset(access_token, 0, sizeof access_token);
            if (err && errlen) snprintf(err, errlen, "could not save OAuth credentials on console");
            return -1;
        }
        net_token_cache_set(access_token, exp);
        memset(access_token, 0, sizeof access_token);

        if (state_out && state_len) snprintf(state_out, state_len, "done");
        return 1;
    }

    char ecode[64] = "", edesc[200] = "";
    extract_oauth_err(resp.data, ecode, sizeof ecode, edesc, sizeof edesc);
    if (resp.data) memset(resp.data, 0, resp.len);
    sb_free(&resp);
    memset(&o, 0, sizeof o);

    if (!strcmp(ecode, "authorization_pending")) {
        if (state_out && state_len) snprintf(state_out, state_len, "pending");
        return 0;
    }
    if (!strcmp(ecode, "slow_down")) {
        flow->interval_s += 5;
        if (state_out && state_len) snprintf(state_out, state_len, "slow_down");
        return 0;
    }
    if (!strcmp(ecode, "access_denied")) {
        if (state_out && state_len) snprintf(state_out, state_len, "denied");
        if (err && errlen) snprintf(err, errlen, "Sign-in was cancelled on Google");
        return -1;
    }
    if (!strcmp(ecode, "expired_token")) {
        if (state_out && state_len) snprintf(state_out, state_len, "expired");
        if (err && errlen) snprintf(err, errlen, "Sign-in code expired — click New code");
        return -1;
    }

    if (state_out && state_len) snprintf(state_out, state_len, "error");
    if (err && errlen) {
        if (edesc[0]) snprintf(err, errlen, "%s", edesc);
        else if (ecode[0]) snprintf(err, errlen, "OAuth error: %s", ecode);
        else snprintf(err, errlen, "OAuth token check failed (HTTP %ld)", status);
    }
    return -1;
}

int yt_oauth_refresh(char *token_out, size_t tok_len, int *invalid_grant_out,
                     char *err, size_t errlen) {
    ensure_urls();
    if (invalid_grant_out) *invalid_grant_out = 0;
    if (token_out && tok_len) token_out[0] = 0;

    yt_oauth o;
    if (net_oauth_get(&o) != 0 || !o.client_id[0] || !o.client_secret[0] || !o.refresh_token[0]) {
        memset(&o, 0, sizeof o);
        if (err && errlen) snprintf(err, errlen, "not signed in — scan QR to sign in to YouTube");
        return -1;
    }

    char ecid[1024], esec[1024], eref[2048], url[640];
    url_encode(o.client_id, ecid, sizeof ecid);
    url_encode(o.client_secret, esec, sizeof esec);
    url_encode(o.refresh_token, eref, sizeof eref);
    snprintf(url, sizeof url, "%s/token", g_oauth_base);

    sbuf form, resp;
    sb_init(&form);
    sb_init(&resp);
    sb_printf(&form, "grant_type=refresh_token&client_id=%s&client_secret=%s&refresh_token=%s",
              ecid, esec, eref);
    memset(esec, 0, sizeof esec);
    memset(eref, 0, sizeof eref);

    CURL *c = curl_easy_init();
    if (!c) {
        memset(&o, 0, sizeof o);
        if (form.data) memset(form.data, 0, form.len);
        sb_free(&form);
        sb_free(&resp);
        if (err && errlen) snprintf(err, errlen, "curl init failed");
        return -1;
    }

    struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
    hdr = curl_slist_append(hdr, "Expect:");

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)form.len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (form.data) memset(form.data, 0, form.len);
    sb_free(&form);

    if (rc != CURLE_OK) {
        memset(&o, 0, sizeof o);
        sb_free(&resp);
        return -1;
    }

    if (status == 200 && resp.data) {
        char access_token[2048] = "";
        char rotated_rt[512] = "";
        const char *v_at = json_find(resp.data, "access_token");
        const char *v_rt = json_find(resp.data, "refresh_token");
        int exp = (int)json_int(json_find(resp.data, "expires_in"), 3600);
        if (v_at) json_string(v_at, access_token, sizeof access_token);
        if (v_rt) json_string(v_rt, rotated_rt, sizeof rotated_rt);
        memset(resp.data, 0, resp.len);
        sb_free(&resp);

        if (!access_token[0]) {
            memset(&o, 0, sizeof o);
            memset(rotated_rt, 0, sizeof rotated_rt);
            if (err && errlen) snprintf(err, errlen, "token refresh returned empty access_token");
            return -1;
        }
        if (rotated_rt[0] && strcmp(rotated_rt, o.refresh_token) != 0) {
            snprintf(o.refresh_token, sizeof o.refresh_token, "%s", rotated_rt);
            net_oauth_set(&o);
        }
        memset(rotated_rt, 0, sizeof rotated_rt);
        memset(&o, 0, sizeof o);

        net_token_cache_set(access_token, exp);
        if (token_out && tok_len) snprintf(token_out, tok_len, "%s", access_token);
        memset(access_token, 0, sizeof access_token);
        return 0;
    }

    char ecode[64] = "", edesc[200] = "";
    extract_oauth_err(resp.data, ecode, sizeof ecode, edesc, sizeof edesc);
    if (resp.data) memset(resp.data, 0, resp.len);
    sb_free(&resp);
    memset(&o, 0, sizeof o);

    if ((status == 400 || status == 401) && !strcmp(ecode, "invalid_grant")) {
        if (invalid_grant_out) *invalid_grant_out = 1;
        net_oauth_clear_tokens();
        if (err && errlen) snprintf(err, errlen, "YouTube sign-in expired — scan QR to sign in again");
        return -1;
    }
    if (err && errlen) {
        if (edesc[0]) snprintf(err, errlen, "%s", edesc);
        else if (ecode[0]) snprintf(err, errlen, "token refresh failed: %s", ecode);
        else snprintf(err, errlen, "token refresh failed (HTTP %ld)", status);
    }
    return -1;
}

int yt_fetch_channel(const char *access_token, char *cid_out, size_t cid_len,
                     char *cname_out, size_t cname_len, int *http_status,
                     char *err, size_t errlen) {
    ensure_urls();
    if (http_status) *http_status = 0;
    if (cid_out && cid_len) cid_out[0] = 0;
    if (cname_out && cname_len) cname_out[0] = 0;
    if (!access_token || !access_token[0]) {
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "missing access token");
        return -1;
    }

    char url[640];
    snprintf(url, sizeof url, "%s/channels?part=snippet&mine=true", g_api_base);

    CURL *c = curl_easy_init();
    if (!c) {
        if (err && errlen) snprintf(err, errlen, "curl init failed");
        return -1;
    }

    struct curl_slist *hdr = append_bearer(NULL, access_token);
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (http_status) *http_status = (int)status;

    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }
    if (status != 200 || !resp.data) {
        if (err && errlen) {
            if (status == 401) snprintf(err, errlen, "YouTube access token rejected (HTTP 401)");
            else if (status == 403) snprintf(err, errlen, "YouTube Data API v3 access denied (ensure YouTube Data API v3 is enabled)");
            else snprintf(err, errlen, "YouTube channel lookup failed (HTTP %ld)", status);
        }
        sb_free(&resp);
        return -1;
    }

    const char *items = json_find(resp.data, "items");
    const char *first = json_array_first(items);
    if (!first) {
        sb_free(&resp);
        if (err && errlen) snprintf(err, errlen, "No YouTube channel found on this Google account — create a channel in YouTube Studio first");
        return -1;
    }

    const char *idv = json_find(first, "id");
    const char *snip = json_find(first, "snippet");
    const char *titlev = json_find(snip, "title");
    if (idv && cid_out && cid_len) json_string(idv, cid_out, cid_len);
    if (titlev && cname_out && cname_len) json_string(titlev, cname_out, cname_len);
    sb_free(&resp);

    if (cid_out && cid_len && !cid_out[0]) {
        if (err && errlen) snprintf(err, errlen, "missing channel ID in YouTube response");
        return -1;
    }
    if (cname_out && cname_len && !cname_out[0]) snprintf(cname_out, cname_len, "YouTube Channel");
    return 0;
}

static const char *lowercase_privacy(const char *p) {
    if (p && !strcasecmp(p, "PUBLIC")) return "public";
    if (p && !strcasecmp(p, "PRIVATE")) return "private";
    return "unlisted";
}

int yt_upload_init(const char *access_token, const yt_video_meta *meta, const char *mime_type,
                   int64_t file_size, char *upload_url_out, size_t url_len,
                   int *http_status, int *quota_exceeded_out, char *err, size_t errlen) {
    ensure_urls();
    if (http_status) *http_status = 0;
    if (quota_exceeded_out) *quota_exceeded_out = 0;
    if (upload_url_out && url_len) upload_url_out[0] = 0;
    if (!access_token || !access_token[0] || !meta || !upload_url_out || !url_len) return -1;

    char url[640];
    snprintf(url, sizeof url, "%s/videos?uploadType=resumable&part=snippet,status,paidProductPlacementDetails",
             g_upload_base);

    const char *title = meta->title[0] ? meta->title : "PS5 Gameplay Capture";
    const char *priv = lowercase_privacy(meta->privacy);
    const char *mime = (mime_type && mime_type[0]) ? mime_type : "video/mp4";

    sbuf jbody, resp;
    sb_init(&jbody);
    sb_init(&resp);
    sb_puts(&jbody, "{\"snippet\":{\"title\":");
    sb_json_str(&jbody, title);
    sb_puts(&jbody, ",\"description\":");
    sb_json_str(&jbody, meta->description);
    sb_puts(&jbody, ",\"categoryId\":\"20\"},\"status\":{\"privacyStatus\":");
    sb_json_str(&jbody, priv);
    sb_printf(&jbody,
              ",\"selfDeclaredMadeForKids\":%s},"
              "\"paidProductPlacementDetails\":{\"hasPaidProductPlacement\":false}}",
              meta->made_for_kids ? "true" : "false");

    char ct_hdr[96], len_hdr[96];
    snprintf(ct_hdr, sizeof ct_hdr, "X-Upload-Content-Type: %s", mime);
    snprintf(len_hdr, sizeof len_hdr, "X-Upload-Content-Length: %lld", (long long)file_size);

    struct curl_slist *hdr = append_bearer(NULL, access_token);
    hdr = curl_slist_append(hdr, "Content-Type: application/json; charset=UTF-8");
    hdr = curl_slist_append(hdr, ct_hdr);
    hdr = curl_slist_append(hdr, len_hdr);

    CURL *c = curl_easy_init();
    if (!c) {
        curl_slist_free_all(hdr);
        sb_free(&jbody);
        sb_free(&resp);
        return -1;
    }

    upload_hdrs uh;
    memset(&uh, 0, sizeof uh);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, jbody.data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)jbody.len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, capture_upload_headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &uh);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    sb_free(&jbody);
    if (http_status) *http_status = (int)status;

    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }
    if (status == 403 && response_has_quota_err(resp.data)) {
        if (quota_exceeded_out) *quota_exceeded_out = 1;
        if (err && errlen) snprintf(err, errlen, "YouTube API daily upload quota reached — resets at midnight Pacific Time");
        sb_free(&resp);
        return -1;
    }
    sb_free(&resp);
    if (status == 401) {
        if (err && errlen) snprintf(err, errlen, "YouTube access token expired (HTTP 401)");
        return -1;
    }
    if ((status != 200 && status != 201) || !uh.location[0]) {
        if (err && errlen) snprintf(err, errlen, "YouTube upload init failed (HTTP %ld)", status);
        return -1;
    }
    if (strlen(uh.location) >= url_len) {
        if (err && errlen) snprintf(err, errlen, "YouTube resumable upload URL too long");
        return -1;
    }
    snprintf(upload_url_out, url_len, "%s", uh.location);
    return 0;
}

int yt_upload_query(const char *access_token, const char *upload_url, int64_t file_size,
                    int64_t *offset_out, int *is_final_out, char *video_id_out, size_t vid_len,
                    int *http_status, char *err, size_t errlen) {
    if (http_status) *http_status = 0;
    if (offset_out) *offset_out = 0;
    if (is_final_out) *is_final_out = 0;
    if (video_id_out && vid_len) video_id_out[0] = 0;
    if (!access_token || !access_token[0] || !upload_url || !upload_url[0]) return -1;

    char range_hdr[96];
    snprintf(range_hdr, sizeof range_hdr, "Content-Range: bytes */%lld", (long long)file_size);

    struct curl_slist *hdr = append_bearer(NULL, access_token);
    hdr = curl_slist_append(hdr, range_hdr);

    CURL *c = curl_easy_init();
    if (!c) {
        curl_slist_free_all(hdr);
        return -1;
    }

    upload_hdrs uh;
    memset(&uh, 0, sizeof uh);
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, upload_url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, capture_upload_headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &uh);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (http_status) *http_status = (int)status;

    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }
    if (status == 308) {
        if (is_final_out) *is_final_out = 0;
        if (offset_out) *offset_out = uh.has_range ? (uh.range_end + 1) : 0;
        sb_free(&resp);
        return 0;
    }
    if (status == 200 || status == 201) {
        if (is_final_out) *is_final_out = 1;
        if (offset_out) *offset_out = file_size;
        if (video_id_out && vid_len && resp.data) {
            const char *idv = json_find(resp.data, "id");
            if (idv) json_string(idv, video_id_out, vid_len);
        }
        sb_free(&resp);
        return 0;
    }

    sb_free(&resp);
    if (err && errlen) snprintf(err, errlen, "YouTube upload status query failed (HTTP %ld)", status);
    return -1;
}

int yt_upload_chunk(const char *access_token, const char *upload_url, FILE *fp,
                    const char *mime_type, int64_t offset, int64_t chunk_len, int64_t file_size,
                    yt_progress_fn cb, void *ud, volatile int *stop_flag,
                    int64_t *acked_offset_out, char *video_id_out, size_t vid_len,
                    int *http_status, int *quota_exceeded_out, char *err, size_t errlen) {
    if (http_status) *http_status = 0;
    if (quota_exceeded_out) *quota_exceeded_out = 0;
    if (acked_offset_out) *acked_offset_out = offset;
    if (video_id_out && vid_len) video_id_out[0] = 0;
    if (!access_token || !access_token[0] || !upload_url || !fp || offset < 0 || chunk_len <= 0 || file_size <= 0)
        return -1;

    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
        if (err && errlen) snprintf(err, errlen, "cannot seek capture file");
        return -1;
    }

    const char *mime = (mime_type && mime_type[0]) ? mime_type : "video/mp4";
    char ct_hdr[96], cr_hdr[128];
    snprintf(ct_hdr, sizeof ct_hdr, "Content-Type: %s", mime);
    snprintf(cr_hdr, sizeof cr_hdr, "Content-Range: bytes %lld-%lld/%lld",
             (long long)offset, (long long)(offset + chunk_len - 1), (long long)file_size);

    struct curl_slist *hdr = append_bearer(NULL, access_token);
    hdr = curl_slist_append(hdr, ct_hdr);
    hdr = curl_slist_append(hdr, cr_hdr);

    CURL *c = curl_easy_init();
    if (!c) {
        curl_slist_free_all(hdr);
        return -1;
    }

    chunk_read_ctx ctx = {fp, chunk_len, cb, ud, stop_flag};
    upload_hdrs uh;
    memset(&uh, 0, sizeof uh);
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, upload_url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)chunk_len);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, read_chunk_cb);
    curl_easy_setopt(c, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 45L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, capture_upload_headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &uh);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (http_status) *http_status = (int)status;

    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }

    int is_final = (offset + chunk_len >= file_size);
    if (status == 308) {
        if (acked_offset_out) {
            if (is_final) *acked_offset_out = uh.has_range ? (uh.range_end + 1) : offset;
            else *acked_offset_out = uh.has_range ? (uh.range_end + 1) : (offset + chunk_len);
        }
        sb_free(&resp);
        return 0;
    }
    if (status == 200 || status == 201) {
        if (video_id_out && vid_len && resp.data) {
            const char *idv = json_find(resp.data, "id");
            if (idv) json_string(idv, video_id_out, vid_len);
        }
        sb_free(&resp);
        if (video_id_out && (!video_id_out[0])) {
            if (err && errlen) snprintf(err, errlen, "missing video id in YouTube upload response");
            return -1;
        }
        if (acked_offset_out) *acked_offset_out = file_size;
        return 0;
    }
    if (status == 403 && response_has_quota_err(resp.data)) {
        if (quota_exceeded_out) *quota_exceeded_out = 1;
        if (err && errlen) snprintf(err, errlen, "YouTube API daily upload quota reached — resets at midnight Pacific Time");
        sb_free(&resp);
        return -1;
    }

    sb_free(&resp);
    if (err && errlen) snprintf(err, errlen, "YouTube chunk upload failed (HTTP %ld)", status);
    return -1;
}

int yt_playlist_add(const char *access_token, const char *playlist_id, const char *video_id,
                    int *http_status, char *err, size_t errlen) {
    ensure_urls();
    if (http_status) *http_status = 0;
    if (!access_token || !access_token[0] || !playlist_id || !playlist_id[0] || !video_id || !video_id[0])
        return -1;

    char url[640];
    snprintf(url, sizeof url, "%s/playlistItems?part=snippet", g_api_base);

    sbuf jbody, resp;
    sb_init(&jbody);
    sb_init(&resp);
    sb_puts(&jbody, "{\"snippet\":{\"playlistId\":");
    sb_json_str(&jbody, playlist_id);
    sb_puts(&jbody, ",\"resourceId\":{\"kind\":\"youtube#video\",\"videoId\":");
    sb_json_str(&jbody, video_id);
    sb_puts(&jbody, "}}}");

    struct curl_slist *hdr = append_bearer(NULL, access_token);
    hdr = curl_slist_append(hdr, "Content-Type: application/json; charset=UTF-8");

    CURL *c = curl_easy_init();
    if (!c) {
        curl_slist_free_all(hdr);
        sb_free(&jbody);
        sb_free(&resp);
        return -1;
    }

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, jbody.data);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)jbody.len);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    sb_free(&jbody);
    sb_free(&resp);
    if (http_status) *http_status = (int)status;

    if (rc != CURLE_OK) return -1;
    if (status == 200 || status == 201) return 0;
    if (err && errlen) snprintf(err, errlen, "playlist add failed (HTTP %ld)", status);
    return -1;
}

int yt_fetch_playlists(const char *access_token, sbuf *items_json_out, int *http_status,
                       char *err, size_t errlen) {
    ensure_urls();
    if (http_status) *http_status = 0;
    if (!access_token || !access_token[0] || !items_json_out) {
        if (http_status) *http_status = 401;
        if (err && errlen) snprintf(err, errlen, "missing access token");
        return -1;
    }

    char url[640];
    snprintf(url, sizeof url, "%s/playlists?part=snippet,contentDetails&mine=true&maxResults=50", g_api_base);

    CURL *c = curl_easy_init();
    if (!c) {
        if (err && errlen) snprintf(err, errlen, "curl init failed");
        return -1;
    }

    struct curl_slist *hdr = append_bearer(NULL, access_token);
    sbuf resp;
    sb_init(&resp);

    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else if (err && errlen) snprintf(err, errlen, "%s", curl_easy_strerror(rc));

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (http_status) *http_status = (int)status;

    if (rc != CURLE_OK) {
        sb_free(&resp);
        return -1;
    }
    if (status != 200 || !resp.data) {
        if (err && errlen) {
            if (status == 401) snprintf(err, errlen, "YouTube access token expired (HTTP 401)");
            else snprintf(err, errlen, "YouTube playlists fetch failed (HTTP %ld)", status);
        }
        sb_free(&resp);
        return -1;
    }

    sb_init(items_json_out);
    sb_puts(items_json_out, "[");
    const char *items = json_find(resp.data, "items");
    int count = 0;
    for (const char *el = json_array_first(items); el; el = json_array_next(el)) {
        char id[64] = "", title[256] = "";
        const char *idv = json_find(el, "id");
        const char *snip = json_find(el, "snippet");
        const char *tv = json_find(snip, "title");
        const char *cd = json_find(el, "contentDetails");
        long long cnt = json_int(json_find(cd, "itemCount"), 0);
        if (!idv || json_string(idv, id, sizeof id) != 0 || !id[0]) continue;
        if (!tv || json_string(tv, title, sizeof title) != 0 || !title[0]) snprintf(title, sizeof title, "%s", id);
        if (count++) sb_puts(items_json_out, ",");
        sb_puts(items_json_out, "{\"id\":");
        sb_json_str(items_json_out, id);
        sb_puts(items_json_out, ",\"title\":");
        sb_json_str(items_json_out, title);
        sb_printf(items_json_out, ",\"count\":%lld}", cnt);
    }
    sb_puts(items_json_out, "]");
    sb_free(&resp);
    return 0;
}
