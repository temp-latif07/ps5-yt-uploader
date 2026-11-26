// PS5 YouTube Uploader: shared declarations.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define APP_VERSION "2.0.0"
#define APP_NAME "ps5-yt-uploader"
#define APP_TITLE_ID "PPSA97631"
#define APP_PORT 8888
#define THREAD_STACK (1024 * 1024) // below 2 MiB: titles/payloads share the flexible pool

typedef struct {
    char channel_id[64];
    char channel_name[160];
    char delegated_session_id[64];
    char session_index[16];
    char api_key[128];
    char client_version[64];
} yt_session;

typedef struct {
    char title[256];
    char description[2048];
    char privacy[16];
    int made_for_kids;
    char playlist_id[64];
} yt_video_meta;

typedef struct {
    char default_privacy[16];
    int made_for_kids;
    int auto_upload;
    int64_t auto_upload_since;
    int chunk_mb;
    int parallel;
    char captures_dir[512];
} app_settings;

typedef struct {
    char path[1024];
    char name[256];
    char title_id[64];
    int64_t size;
    int64_t mtime;
} clip_entry;

// ---- platform (platform.c) ----
void plat_init(void);
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *plat_app_dir(void);
const char *plat_captures_dir(void);
const char *plat_mode(void);
int plat_path_allowed(const char *path);
void plat_local_ip(char *out, size_t len);
int plat_start_thread(void *(*fn)(void *), void *arg);
int64_t plat_now_ms(void);
void plat_sleep_ms(int ms);

// ---- utilities (util.c) ----
typedef struct {
    char *data;
    size_t len, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
int sb_add(sbuf *b, const void *data, size_t len);
int sb_puts(sbuf *b, const char *s);
int sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(sbuf *b, const char *s);
int mkdir_p(const char *path);
void sanitize_name(char *s);
int url_decode(char *s);
const char *form_get(const char *form, const char *key, char *out, size_t len);
char *form_get_alloc(const char *form, const char *key, size_t max_len);
void url_encode(const char *in, char *out, size_t len);
char *str_trim(char *s);
int is_video_filename(const char *name);

// ---- minimal JSON reader (json.c) ----
const char *json_find(const char *obj, const char *key);
int json_string(const char *val, char *out, size_t len);
int json_bool(const char *val);
long long json_int(const char *val, long long def);
const char *json_array_first(const char *arr);
const char *json_array_next(const char *elem);

// ---- SHA-1 & SAPISIDHASH (sha1.c) ----
void sha1_hex(const unsigned char *data, size_t len, char hex_out[41]);
int sapisid_hash(const char *sapisid, const char *origin, long ts, char *out, size_t len);

// ---- cookies, sessions & HTTPS setup (net.c) ----
void net_init(void);
const char *net_cookie_path(void);
const char *net_session_path(void);
int net_has_cookies(void);
int net_normalize_cookies(const char *raw, sbuf *netscape_out, char *err, size_t errlen);
int net_extract_cookie(const char *jar_path, const char *name, char *out, size_t len);
int net_write_temp_jar(const char *netscape_data, char *tmp_path_out, size_t len);
int net_commit_jar(const char *tmp_path);
int net_snapshot_jar(char *tmp_path_out, size_t len);
int net_session_get(yt_session *out);
int net_session_set(const yt_session *s);
void net_auth_clear(void);
void net_apply(void *curl_handle);

// ---- YouTube Studio client (youtube.c) ----
void yt_init(void);
// Validates cookie jar live against Studio and bootstraps session metadata:
// 1 = valid, 0 = refused/expired, -1 = transport/server error.
int yt_validate_and_bootstrap(const char *jar_path, int commit_jar, yt_session *sess, char *err, size_t errlen);
int yt_scotty_init(const yt_session *s, const char *frontend_id, int64_t file_size,
                   char *upload_url_out, size_t url_len, int *http_status, char *err, size_t errlen);
int yt_scotty_query(const yt_session *s, const char *upload_url, int64_t *offset_out, int *is_final_out,
                    char *scotty_id_out, size_t id_len, int *http_status, char *err, size_t errlen);
typedef void (*yt_progress_fn)(int64_t chunk_sent, void *ud);
int yt_scotty_upload_chunk(const yt_session *s, const char *upload_url, FILE *fp, int64_t offset, int64_t chunk_len,
                           int is_final, yt_progress_fn cb, void *ud, volatile int *stop_flag,
                           char *scotty_id_out, size_t id_len, int *http_status, char *err, size_t errlen);
int yt_create_video(const yt_session *s, const char *frontend_id, const char *scotty_id, const yt_video_meta *meta,
                    char *video_id_out, size_t vid_len, int *http_status, char *err, size_t errlen);
int yt_update_metadata(const yt_session *s, const char *video_id, const yt_video_meta *meta,
                       int *http_status, char *err, size_t errlen);

// ---- clips, folder browser & upload history (clips.c) ----
void clips_init(void);
int clips_history_lookup(const char *path, char *vid_out, size_t vid_len);
int clips_history_mark(const char *path, const char *video_id, const char *title);
int clips_scan_array(const char *dir, clip_entry **out, int *count_out);
int clips_scan_json(const char *dir, sbuf *out, char *err, size_t errlen);
int clips_list_dirs_json(const char *path, sbuf *out, char *err, size_t errlen);

// ---- persistent upload queue & optional watcher (upload.c) ----
void upload_init(void);
void upload_shutdown(void);
void upload_settings_get(app_settings *out);
int upload_settings_set(const app_settings *in, char *err, size_t errlen);
int upload_add(const char *path, const yt_video_meta *meta, char *err, size_t errlen);
void upload_jobs_json(sbuf *out);
int upload_pause(int id);
int upload_resume(int id);
int upload_cancel(int id);
int upload_remove(int id);
void upload_clear_finished(void);

// ---- web server (httpd.c / api.c) ----
typedef struct {
    int fd;
    char method[8];
    char path[1024];
    char query[4096];
    char range[128];
    char *body;
    size_t body_len;
    int has_app_header; // X-PS5YT present (blocks cross-site form posts)
} http_req;

int httpd_run(int port);
void httpd_stop(void);
void http_send(int fd, int status, const char *ctype, const void *body, size_t len, const char *extra_headers);
void http_send_json(int fd, int status, const sbuf *body);
int http_send_raw(int fd, const void *data, size_t len);
void api_handle(http_req *req);

// ---- embedded assets (generated by tools/embed.py) ----
typedef struct {
    const char *name;
    const char *ctype;
    const unsigned char *data;
    size_t len;
} asset;

const asset *asset_find(const char *name);
extern const unsigned char cacert_pem[];
extern const size_t cacert_pem_len;
