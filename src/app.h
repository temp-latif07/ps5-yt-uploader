// PS5 YouTube Uploader: shared declarations.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define APP_VERSION "2.0.1"
#define APP_NAME "ps5-yt-uploader"
#define APP_TITLE_ID "PPSA97631"
#define APP_PORT 8085
#define THREAD_STACK (1024 * 1024) // below 2 MiB: titles/payloads share the flexible pool

typedef struct {
    char client_id[256];
    char client_secret[256];
    char refresh_token[512];
    char channel_id[64];
    char channel_name[160];
} yt_oauth;

typedef struct {
    char device_code[512];
    char user_code[32];
    char verification_url[256];
    int interval_s;
    int expires_in;
    int64_t expires_at_ms;
} yt_device_flow;

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

// ---- OAuth credentials, token cache & HTTPS setup (net.c) ----
void net_init(void);
const char *net_oauth_path(void);
int net_oauth_get(yt_oauth *out);
int net_oauth_set(const yt_oauth *o);
int net_oauth_has_client(void);
int net_oauth_is_logged_in(void);
void net_oauth_clear_tokens(void);
void net_oauth_clear_all(void);
int net_parse_client_json(const char *json, char *cid, size_t cid_len,
                          char *csec, size_t csec_len, char *err, size_t errlen);
int net_token_cache_get(char *out, size_t len);
void net_token_cache_set(const char *token, int expires_in_s);
void net_token_cache_clear(void);
void net_apply(void *curl_handle);

// ---- Google OAuth Device Flow & YouTube Data API v3 (youtube.c) ----
void yt_init(void);
int yt_oauth_start_device(yt_device_flow *out, char *err, size_t errlen);
// Returns 1 = authorized (oauth.txt saved & token cached), 0 = still waiting ("pending"/"slow_down"),
// -1 = terminal ("denied"/"expired"/"error"). state_out always populated.
int yt_oauth_poll_device(yt_device_flow *flow, char *state_out, size_t state_len,
                         char *err, size_t errlen);
// Refreshes access token using stored refresh_token; caches token & persists rotated refresh_token.
// Sets *invalid_grant_out = 1 and clears stored tokens if Google returns invalid_grant.
int yt_oauth_refresh(char *token_out, size_t tok_len, int *invalid_grant_out,
                     char *err, size_t errlen);
int yt_fetch_channel(const char *access_token, char *cid_out, size_t cid_len,
                     char *cname_out, size_t cname_len, int *http_status,
                     char *err, size_t errlen);
int yt_upload_init(const char *access_token, const yt_video_meta *meta, const char *mime_type,
                   int64_t file_size, char *upload_url_out, size_t url_len,
                   int *http_status, int *quota_exceeded_out, char *err, size_t errlen);
int yt_upload_query(const char *access_token, const char *upload_url, int64_t file_size,
                    int64_t *offset_out, int *is_final_out, char *video_id_out, size_t vid_len,
                    int *http_status, char *err, size_t errlen);
typedef void (*yt_progress_fn)(int64_t chunk_sent, void *ud);
int yt_upload_chunk(const char *access_token, const char *upload_url, FILE *fp,
                    const char *mime_type, int64_t offset, int64_t chunk_len, int64_t file_size,
                    yt_progress_fn cb, void *ud, volatile int *stop_flag,
                    int64_t *acked_offset_out, char *video_id_out, size_t vid_len,
                    int *http_status, int *quota_exceeded_out, char *err, size_t errlen);
int yt_playlist_add(const char *access_token, const char *playlist_id, const char *video_id,
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
