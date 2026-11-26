// JSON API routes, Range video streaming, and embedded static files.
#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void json_err(int fd, int status, const char *msg) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"ok\":false,\"error\":");
    sb_json_str(&b, msg && msg[0] ? msg : "error");
    sb_puts(&b, "}");
    http_send_json(fd, status, &b);
    sb_free(&b);
}

static void json_ok(int fd) {
    sbuf b = {(char *)"{\"ok\":true}", 11, 0};
    http_send_json(fd, 200, &b);
}

static int parse_bool_str(const char *s, int def) {
    if (!s || !s[0]) return def;
    if (!strcasecmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on")) return 1;
    if (!strcasecmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcasecmp(s, "off")) return 0;
    return def;
}

static int current_listen_port(void) {
    const char *env = getenv("PS5YT_PORT");
    return (env && atoi(env) > 0) ? atoi(env) : APP_PORT;
}

static void route_status(http_req *r) {
    int logged = net_has_cookies();
    yt_session sess;
    memset(&sess, 0, sizeof sess);
    char auth_err[300] = "";

    if (logged) {
        if (net_session_get(&sess) != 0 || !sess.channel_name[0] || !sess.api_key[0] || !sess.channel_id[0]) {
            char snap[640];
            if (net_snapshot_jar(snap, sizeof snap) == 0) {
                int rc = yt_validate_and_bootstrap(snap, 1, &sess, auth_err, sizeof auth_err);
                unlink(snap);
                if (rc == 1) net_session_set(&sess);
                else if (rc == 0) logged = 0;
            }
        }
    }

    app_settings st;
    upload_settings_get(&st);
    char ip[64];
    plat_local_ip(ip, sizeof ip);

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"version\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"logged_in\":%s,\"cookie_file\":",
              APP_VERSION, plat_mode(), ip, current_listen_port(), logged ? "true" : "false");
    sb_json_str(&b, net_cookie_path());
    sb_puts(&b, ",\"channel\":");
    if (logged) {
        sb_puts(&b, "{\"name\":");
        sb_json_str(&b, sess.channel_name);
        sb_puts(&b, ",\"id\":");
        sb_json_str(&b, sess.channel_id);
        sb_puts(&b, ",\"delegated_session_id\":");
        sb_json_str(&b, sess.delegated_session_id);
        sb_puts(&b, ",\"session_index\":");
        sb_json_str(&b, sess.session_index[0] ? sess.session_index : "0");
        sb_puts(&b, "}");
    } else {
        sb_puts(&b, "null");
    }
    sb_puts(&b, ",\"auth_error\":");
    sb_json_str(&b, auth_err);
    sb_puts(&b, ",\"captures_dir\":");
    sb_json_str(&b, st.captures_dir);
    sb_puts(&b, ",\"settings\":{\"default_privacy\":");
    sb_json_str(&b, st.default_privacy);
    sb_printf(&b, ",\"made_for_kids\":%s,\"auto_upload\":%s,\"auto_upload_since\":%lld,\"chunk_mb\":%d,\"parallel\":%d,\"captures_dir\":",
              st.made_for_kids ? "true" : "false",
              st.auto_upload ? "true" : "false",
              (long long)st.auto_upload_since,
              st.chunk_mb, st.parallel);
    sb_json_str(&b, st.captures_dir);
    sb_puts(&b, "}}");

    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_login(http_req *r) {
    char *raw = form_get_alloc(r->body, "cookies", 256 * 1024);
    if (!raw || !raw[0]) {
        free(raw);
        json_err(r->fd, 400, "paste YouTube Studio cookies or select a cookies.txt file");
        return;
    }

    sbuf netscape;
    char err[300] = "";
    int norm_rc = net_normalize_cookies(raw, &netscape, err, sizeof err);
    memset(raw, 0, strlen(raw));
    free(raw);
    if (norm_rc != 0) {
        json_err(r->fd, 400, err);
        return;
    }

    char tmp_jar[640];
    int wr_rc = net_write_temp_jar(netscape.data, tmp_jar, sizeof tmp_jar);
    if (netscape.data) memset(netscape.data, 0, netscape.len);
    sb_free(&netscape);
    if (wr_rc != 0) {
        json_err(r->fd, 500, "could not write temporary cookie file");
        return;
    }

    yt_session sess;
    memset(&sess, 0, sizeof sess);
    form_get(r->body, "channel_id", sess.channel_id, sizeof sess.channel_id);
    form_get(r->body, "delegated_session_id", sess.delegated_session_id, sizeof sess.delegated_session_id);
    form_get(r->body, "session_index", sess.session_index, sizeof sess.session_index);
    str_trim(sess.channel_id);
    str_trim(sess.delegated_session_id);
    str_trim(sess.session_index);

    int rc = yt_validate_and_bootstrap(tmp_jar, 1, &sess, err, sizeof err);
    unlink(tmp_jar);

    if (rc == 1) {
        if (net_session_set(&sess) != 0) {
            json_err(r->fd, 500, "could not save session metadata on console");
            return;
        }
        plat_log("login: authenticated YouTube channel %s (%s)", sess.channel_name, sess.channel_id);
        route_status(r);
        return;
    }

    plat_log("login: failed (%s)", err[0] ? err : "rejected");
    json_err(r->fd, rc == 0 ? 401 : 502, err[0] ? err : "YouTube login failed");
}

static void route_logout(http_req *r) {
    net_auth_clear();
    plat_log("logout: cleared stored YouTube cookies and session");
    json_ok(r->fd);
}

static void route_clips(http_req *r) {
    char dir[512] = "";
    form_get(r->query, "dir", dir, sizeof dir);
    if (!dir[0]) {
        app_settings st;
        upload_settings_get(&st);
        snprintf(dir, sizeof dir, "%s", st.captures_dir);
    }
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;

    sbuf b;
    char err[300] = "";
    if (clips_scan_json(dir, &b, err, sizeof err) != 0) {
        json_err(r->fd, plat_path_allowed(dir) ? 500 : 403, err);
        return;
    }
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_dirs(http_req *r) {
    char path[1024] = "";
    form_get(r->query, "path", path, sizeof path);
    sbuf b;
    char err[300] = "";
    if (clips_list_dirs_json(path, &b, err, sizeof err) != 0) {
        json_err(r->fd, (path[0] && !plat_path_allowed(path)) ? 403 : 404, err);
        return;
    }
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static int parse_range_hdr(const char *hdr, int64_t total, int64_t *start, int64_t *end) {
    if (!hdr || strncasecmp(hdr, "bytes=", 6) != 0) return 0; // no Range
    const char *p = hdr + 6;
    while (*p == ' ' || *p == '\t') p++;
    if (total <= 0) return -1;

    if (*p == '-') {
        char *ep = NULL;
        long long suf = strtoll(p + 1, &ep, 10);
        if (ep == p + 1 || suf <= 0) return -1;
        *start = suf >= total ? 0 : total - suf;
        *end = total - 1;
        return 1;
    }

    char *ep = NULL;
    long long s = strtoll(p, &ep, 10);
    if (ep == p || s < 0 || *ep != '-') return -1;
    p = ep + 1;
    long long e = total - 1;
    if (*p >= '0' && *p <= '9') {
        e = strtoll(p, &ep, 10);
        if (e >= total) e = total - 1;
    }
    if (s >= total || s > e) return -1;
    *start = s;
    *end = e;
    return 1;
}

static void route_stream(http_req *r) {
    char path[1024] = "";
    form_get(r->query, "path", path, sizeof path);
    if (!path[0] || !plat_path_allowed(path) || !is_video_filename(path)) {
        json_err(r->fd, 403, "streaming is restricted to allowed video files");
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        json_err(r->fd, 404, "video file not found");
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        json_err(r->fd, 404, strerror(errno));
        return;
    }

    int64_t total = (int64_t)st.st_size;
    int64_t start = 0, end = total > 0 ? total - 1 : 0;
    int rrc = parse_range_hdr(r->range, total, &start, &end);
    if (rrc < 0) {
        fclose(fp);
        char extra[96];
        snprintf(extra, sizeof extra, "Content-Range: bytes */%lld\r\n", (long long)total);
        http_send(r->fd, 416, "text/plain", "", 0, extra);
        return;
    }

    int64_t len = total > 0 ? (end - start + 1) : 0;
    size_t plen = strlen(path);
    const char *ctype = (plen > 5 && !strcasecmp(path + plen - 5, ".webm")) ? "video/webm" : "video/mp4";

    char head[512];
    int hlen;
    if (rrc == 1) {
        hlen = snprintf(head, sizeof head,
                        "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                        "Content-Range: bytes %lld-%lld/%lld\r\nAccept-Ranges: bytes\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                        ctype, (long long)len, (long long)start, (long long)end, (long long)total);
    } else {
        hlen = snprintf(head, sizeof head,
                        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                        "Accept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                        ctype, (long long)len);
    }

    if (hlen > 0 && http_send_raw(r->fd, head, (size_t)hlen) == 0 && len > 0) {
        if (fseeko(fp, (off_t)start, SEEK_SET) == 0) {
            char *buf = malloc(65536);
            if (buf) {
                int64_t rem = len;
                while (rem > 0) {
                    size_t want = rem > 65536 ? 65536 : (size_t)rem;
                    size_t got = fread(buf, 1, want, fp);
                    if (got == 0) break;
                    if (http_send_raw(r->fd, buf, got) != 0) break;
                    rem -= (int64_t)got;
                }
                free(buf);
            }
        }
    }
    fclose(fp);
}

static void route_upload(http_req *r) {
    if (!net_has_cookies()) {
        json_err(r->fd, 401, "not logged in — sign in to YouTube first");
        return;
    }
    char path[1024] = "", mfk_s[16] = "", err[300] = "";
    form_get(r->body, "path", path, sizeof path);
    if (!path[0] || !plat_path_allowed(path) || !is_video_filename(path)) {
        json_err(r->fd, 403, "capture file path is outside allowed video files");
        return;
    }

    yt_video_meta meta;
    memset(&meta, 0, sizeof meta);
    form_get(r->body, "title", meta.title, sizeof meta.title);
    form_get(r->body, "description", meta.description, sizeof meta.description);
    form_get(r->body, "privacy", meta.privacy, sizeof meta.privacy);
    form_get(r->body, "made_for_kids", mfk_s, sizeof mfk_s);
    form_get(r->body, "playlist_id", meta.playlist_id, sizeof meta.playlist_id);
    str_trim(meta.title);
    str_trim(meta.playlist_id);
    meta.made_for_kids = parse_bool_str(mfk_s, 0);

    if (meta.privacy[0] && strcmp(meta.privacy, "UNLISTED") &&
        strcmp(meta.privacy, "PUBLIC") && strcmp(meta.privacy, "PRIVATE")) {
        json_err(r->fd, 400, "privacy must be UNLISTED, PUBLIC or PRIVATE");
        return;
    }

    int jid = upload_add(path, &meta, err, sizeof err);
    if (jid < 0) {
        json_err(r->fd, 400, err);
        return;
    }
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"id\":%d}", jid);
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_jobs(http_req *r) {
    sbuf b;
    upload_jobs_json(&b);
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_job_action(http_req *r, const char *action) {
    char id[32] = "";
    form_get(r->body, "id", id, sizeof id);
    int jid = atoi(id), rc = 0;
    if (!strcmp(action, "pause")) rc = upload_pause(jid);
    else if (!strcmp(action, "resume")) rc = upload_resume(jid);
    else if (!strcmp(action, "cancel")) rc = upload_cancel(jid);
    else if (!strcmp(action, "remove")) rc = upload_remove(jid);
    else if (!strcmp(action, "clear")) upload_clear_finished();
    else {
        json_err(r->fd, 404, "unknown job action");
        return;
    }
    if (rc) json_err(r->fd, 409, "not possible in the upload's current state");
    else json_ok(r->fd);
}

static void route_settings(http_req *r) {
    app_settings st;
    upload_settings_get(&st);

    char val[512];
    if (form_get(r->body, "default_privacy", val, sizeof val))
        snprintf(st.default_privacy, sizeof st.default_privacy, "%s", val);
    if (form_get(r->body, "made_for_kids", val, sizeof val))
        st.made_for_kids = parse_bool_str(val, st.made_for_kids);
    if (form_get(r->body, "auto_upload", val, sizeof val))
        st.auto_upload = parse_bool_str(val, st.auto_upload);
    if (form_get(r->body, "chunk_mb", val, sizeof val) && val[0])
        st.chunk_mb = atoi(val);
    if (form_get(r->body, "parallel", val, sizeof val) && val[0])
        st.parallel = atoi(val);
    if (form_get(r->body, "captures_dir", val, sizeof val) && val[0]) {
        size_t n = strlen(val);
        while (n > 1 && val[n - 1] == '/') val[--n] = 0;
        snprintf(st.captures_dir, sizeof st.captures_dir, "%s", val);
    }

    char err[300] = "";
    if (upload_settings_set(&st, err, sizeof err) != 0) {
        json_err(r->fd, 400, err);
        return;
    }
    route_status(r);
}

static void route_static(http_req *r) {
    const char *name = r->path[1] ? r->path + 1 : "index.html";
    const asset *a = asset_find(name);
    if (!a) {
        http_send(r->fd, 404, "text/plain", "not found", 9, NULL);
        return;
    }
    http_send(r->fd, 200, a->ctype, a->data, a->len, NULL);
}

void api_handle(http_req *r) {
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if (strncmp(r->path, "/api/", 5) != 0) {
        if (get) route_static(r);
        else http_send(r->fd, 405, "text/plain", "method not allowed", 18, NULL);
        return;
    }
    const char *p = r->path + 5;
    if (post && !r->has_app_header) {
        json_err(r->fd, 403, "missing X-PS5YT header");
        return;
    }
    if (get && !strcmp(p, "status")) route_status(r);
    else if (get && !strcmp(p, "clips")) route_clips(r);
    else if (get && !strcmp(p, "dirs")) route_dirs(r);
    else if (get && !strcmp(p, "stream")) route_stream(r);
    else if (get && !strcmp(p, "jobs")) route_jobs(r);
    else if (post && !strcmp(p, "login")) route_login(r);
    else if (post && !strcmp(p, "logout")) route_logout(r);
    else if (post && !strcmp(p, "upload")) route_upload(r);
    else if (post && !strcmp(p, "settings")) route_settings(r);
    else if (post && !strncmp(p, "jobs/", 5)) route_job_action(r, p + 5);
    else if (post && !strcmp(p, "quit")) {
        json_ok(r->fd);
        plat_log("quit requested from the web UI");
        httpd_stop();
    } else {
        json_err(r->fd, 404, "unknown API call");
    }
}
