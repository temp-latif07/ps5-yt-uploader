// Persistent resumable upload queue and optional off-by-default capture watcher.
#include "app.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { U_QUEUED, U_STARTING, U_UPLOADING, U_FINALIZING, U_DONE, U_PAUSED, U_FAILED, U_CANCELLED };
static const char *state_names[] = {
    "queued", "starting", "uploading", "finalizing", "done", "paused", "failed", "cancelled"
};

#define MAX_JOBS 1000

typedef struct {
    int id, state;
    volatile int stop; // 1 = pause, 2 = cancel
    char path[1024], name[256];
    yt_video_meta meta;
    char upload_url[1024];
    char video_id[64];
    int64_t done, total, start_done, started_ms, finished_ms;
    double speed;
    char err[300], note[256];
} upload_job;

typedef struct {
    char path[1024];
    int64_t last_size;
    int stable_scans;
    int baselined;
} watch_entry;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static upload_job *g_jobs[MAX_JOBS];
static int g_njobs, g_next_id = 1, g_running;
static volatile int g_quit;
static int g_watcher_alive;
static app_settings g_settings;

static watch_entry *g_watch;
static int g_nwatch, g_capwatch;

// ---------- settings.txt & queue.tsv persistence ----------

static void save_settings_locked(void) {
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/settings.txt", plat_app_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "default_privacy=%s\nmade_for_kids=%d\nauto_upload=%d\n"
            "auto_upload_since=%lld\nchunk_mb=%d\nparallel=%d\ncaptures_dir=%s\n",
            g_settings.default_privacy, g_settings.made_for_kids ? 1 : 0,
            g_settings.auto_upload ? 1 : 0, (long long)g_settings.auto_upload_since,
            g_settings.chunk_mb, g_settings.parallel, g_settings.captures_dir);
    fclose(f);
    rename(tmp, path);
}

static void load_settings(void) {
    memset(&g_settings, 0, sizeof g_settings);
    snprintf(g_settings.default_privacy, sizeof g_settings.default_privacy, "UNLISTED");
    g_settings.chunk_mb = 8;
    g_settings.parallel = 1;
    snprintf(g_settings.captures_dir, sizeof g_settings.captures_dir, "%s", plat_captures_dir());

    char path[600], line[600];
    snprintf(path, sizeof path, "%s/settings.txt", plat_app_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "default_privacy") && (!strcmp(v, "UNLISTED") || !strcmp(v, "PUBLIC") || !strcmp(v, "PRIVATE")))
            snprintf(g_settings.default_privacy, sizeof g_settings.default_privacy, "%s", v);
        else if (!strcmp(k, "made_for_kids")) g_settings.made_for_kids = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "auto_upload")) g_settings.auto_upload = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "auto_upload_since")) g_settings.auto_upload_since = (int64_t)strtoll(v, NULL, 10);
        else if (!strcmp(k, "chunk_mb")) {
            int cm = atoi(v);
            if (cm == 4 || cm == 8 || cm == 16) g_settings.chunk_mb = cm;
        } else if (!strcmp(k, "parallel")) {
            int p = atoi(v);
            if (p >= 1 && p <= 2) g_settings.parallel = p;
        } else if (!strcmp(k, "captures_dir") && v[0] && plat_path_allowed(v)) {
            struct stat cst;
            if (strcmp(v, "/user/av_content/video") != 0 || (stat(v, &cst) == 0 && S_ISDIR(cst.st_mode))) {
                snprintf(g_settings.captures_dir, sizeof g_settings.captures_dir, "%s", v);
            }
        }
    }
    fclose(f);
}

static void write_escaped(FILE *f, const char *s, int last) {
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\\') fputs("\\\\", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') fputs("\\r", f);
        else fputc(*p, f);
    }
    fputc(last ? '\n' : '\t', f);
}

static void unescape_field(char *dst, size_t len, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < len; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 't') dst[o++] = '\t';
            else if (*p == 'n') dst[o++] = '\n';
            else if (*p == 'r') dst[o++] = '\r';
            else dst[o++] = *p;
        } else {
            dst[o++] = *p;
        }
    }
    if (len) dst[o] = 0;
}

static void save_queue_locked(void) {
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/queue.tsv", plat_app_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_njobs; i++) {
        upload_job *j = g_jobs[i];
        if (j->state == U_DONE || j->state == U_CANCELLED) continue;
        int st = (j->state == U_FAILED) ? U_FAILED : (j->state == U_PAUSED) ? U_PAUSED : U_QUEUED;
        fprintf(f, "%d\t", st);
        write_escaped(f, j->path, 0);
        write_escaped(f, j->meta.title, 0);
        write_escaped(f, j->meta.description, 0);
        write_escaped(f, j->meta.privacy, 0);
        fprintf(f, "%d\t", j->meta.made_for_kids ? 1 : 0);
        write_escaped(f, j->meta.playlist_id, 0);
        write_escaped(f, "", 0); // reserved slot matching 12-column queue.tsv layout
        write_escaped(f, j->upload_url, 0);
        fprintf(f, "%lld\t%lld\t", (long long)j->done, (long long)j->total);
        write_escaped(f, j->err, 1);
    }
    fclose(f);
    rename(tmp, path);
}

static upload_job *new_job_locked(void) {
    if (g_njobs == MAX_JOBS) {
        int i;
        for (i = 0; i < g_njobs; i++)
            if (g_jobs[i]->state >= U_DONE && g_jobs[i]->state != U_PAUSED) break;
        if (i == g_njobs) return NULL;
        free(g_jobs[i]);
        memmove(g_jobs + i, g_jobs + i + 1, (size_t)(g_njobs - i - 1) * sizeof *g_jobs);
        g_njobs--;
    }
    upload_job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->id = g_next_id++;
    g_jobs[g_njobs++] = j;
    return j;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void load_queue(void) {
    char path[600], line[8192];
    snprintf(path, sizeof path, "%s/queue.tsv", plat_app_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;
        char *cols[12];
        int ncol = 0;
        char *p = line;
        cols[ncol++] = p;
        while (*p && ncol < 12) {
            if (*p == '\t') { *p = 0; cols[ncol++] = p + 1; }
            p++;
        }
        if (ncol < 11) continue;
        upload_job *j = new_job_locked();
        if (!j) break;
        int st = atoi(cols[0]);
        j->state = (st == U_FAILED) ? U_FAILED : (st == U_PAUSED) ? U_PAUSED : U_QUEUED;
        unescape_field(j->path, sizeof j->path, cols[1]);
        snprintf(j->name, sizeof j->name, "%s", base_name(j->path));
        unescape_field(j->meta.title, sizeof j->meta.title, cols[2]);
        unescape_field(j->meta.description, sizeof j->meta.description, cols[3]);
        unescape_field(j->meta.privacy, sizeof j->meta.privacy, cols[4]);
        j->meta.made_for_kids = atoi(cols[5]) ? 1 : 0;
        unescape_field(j->meta.playlist_id, sizeof j->meta.playlist_id, cols[6]);
        unescape_field(j->upload_url, sizeof j->upload_url, cols[8]);
        j->done = (int64_t)strtoll(cols[9], NULL, 10);
        j->total = (int64_t)strtoll(cols[10], NULL, 10);
        if (ncol >= 12) unescape_field(j->err, sizeof j->err, cols[11]);
    }
    fclose(f);
}

// ---------- worker execution ----------

static void schedule_locked(void);

static const char *mime_for_path(const char *path) {
    size_t n = strlen(path);
    if (n > 5 && !strcasecmp(path + n - 5, ".webm")) return "video/webm";
    return "video/mp4";
}

static int ensure_access_token(char *tok, size_t tok_len, int force_refresh,
                               int *invalid_grant, char *err, size_t errlen) {
    if (invalid_grant) *invalid_grant = 0;
    if (!force_refresh && net_token_cache_get(tok, tok_len) == 0) return 0;
    net_token_cache_clear();
    return yt_oauth_refresh(tok, tok_len, invalid_grant, err, errlen);
}

static void on_chunk_progress(int64_t delta, void *ud) {
    upload_job *j = ud;
    pthread_mutex_lock(&g_lock);
    j->done += delta;
    int64_t elapsed = plat_now_ms() - j->started_ms;
    if (elapsed > 250 && j->done >= j->start_done) {
        j->speed = (double)(j->done - j->start_done) / ((double)elapsed / 1000.0);
    }
    pthread_mutex_unlock(&g_lock);
}

static void fail_job(upload_job *j, const char *err) {
    pthread_mutex_lock(&g_lock);
    j->state = U_FAILED;
    j->speed = 0;
    j->finished_ms = plat_now_ms();
    snprintf(j->err, sizeof j->err, "%s", err && err[0] ? err : "upload failed");
    save_queue_locked();
    g_running--;
    schedule_locked();
    pthread_mutex_unlock(&g_lock);
    plat_notify("YouTube: upload failed (%s) — %s", j->name, j->err);
}

static void *upload_worker(void *arg) {
    upload_job *j = arg;
    char err[300] = "";
    char access_token[2048] = "";
    int invalid_grant = 0;

    if (!net_oauth_is_logged_in()) {
        fail_job(j, "not signed in — scan QR to sign in to YouTube");
        return NULL;
    }
    if (ensure_access_token(access_token, sizeof access_token, 0, &invalid_grant, err, sizeof err) != 0) {
        memset(access_token, 0, sizeof access_token);
        fail_job(j, err[0] ? err : "YouTube sign-in expired — scan QR to sign in again");
        return NULL;
    }

    struct stat st;
    if (stat(j->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        memset(access_token, 0, sizeof access_token);
        fail_job(j, "capture file is missing or empty");
        return NULL;
    }
    FILE *fp = fopen(j->path, "rb");
    if (!fp) {
        memset(access_token, 0, sizeof access_token);
        fail_job(j, strerror(errno));
        return NULL;
    }

    pthread_mutex_lock(&g_lock);
    j->total = (int64_t)st.st_size;
    int chunk_mb = g_settings.chunk_mb > 0 ? g_settings.chunk_mb : 8;
    yt_video_meta meta = j->meta;
    char up_url[1024];
    snprintf(up_url, sizeof up_url, "%s", j->upload_url);
    pthread_mutex_unlock(&g_lock);

    const char *mime = mime_for_path(j->path);
    int64_t offset = 0;
    int already_final = 0;
    char video_id[64] = "";
    int http_status = 0;
    int quota_exceeded = 0;

    for (int reinit = 0; reinit < 2; reinit++) {
        if (j->stop) break;
        if (up_url[0]) {
            int is_final = 0;
            int qrc = yt_upload_query(access_token, up_url, j->total, &offset, &is_final,
                                      video_id, sizeof video_id, &http_status, err, sizeof err);
            if (qrc != 0 && http_status == 401) {
                if (ensure_access_token(access_token, sizeof access_token, 1, &invalid_grant, err, sizeof err) == 0) {
                    qrc = yt_upload_query(access_token, up_url, j->total, &offset, &is_final,
                                          video_id, sizeof video_id, &http_status, err, sizeof err);
                } else {
                    fclose(fp);
                    memset(access_token, 0, sizeof access_token);
                    fail_job(j, err[0] ? err : "YouTube sign-in expired — scan QR to sign in again");
                    return NULL;
                }
            }
            if (http_status == 401) {
                fclose(fp);
                memset(access_token, 0, sizeof access_token);
                fail_job(j, "YouTube sign-in expired — scan QR to sign in again");
                return NULL;
            }
            if (http_status == 404 || http_status == 410) {
                plat_log("upload #%d: old upload session expired (HTTP %d), starting fresh", j->id, http_status);
                up_url[0] = 0;
                offset = 0;
                pthread_mutex_lock(&g_lock);
                j->upload_url[0] = 0;
                j->done = 0;
                save_queue_locked();
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            if (qrc != 0) {
                fclose(fp);
                memset(access_token, 0, sizeof access_token);
                fail_job(j, err[0] ? err : "cannot resume upload session");
                return NULL;
            }
            if (is_final && video_id[0]) {
                already_final = 1;
                offset = j->total;
            }
            break;
        } else {
            int irc = yt_upload_init(access_token, &meta, mime, j->total, up_url, sizeof up_url,
                                     &http_status, &quota_exceeded, err, sizeof err);
            if (irc != 0 && http_status == 401) {
                if (ensure_access_token(access_token, sizeof access_token, 1, &invalid_grant, err, sizeof err) == 0) {
                    irc = yt_upload_init(access_token, &meta, mime, j->total, up_url, sizeof up_url,
                                         &http_status, &quota_exceeded, err, sizeof err);
                } else {
                    fclose(fp);
                    memset(access_token, 0, sizeof access_token);
                    fail_job(j, err[0] ? err : "YouTube sign-in expired — scan QR to sign in again");
                    return NULL;
                }
            }
            if (irc != 0) {
                fclose(fp);
                memset(access_token, 0, sizeof access_token);
                fail_job(j, err[0] ? err : "failed to initialize YouTube upload");
                return NULL;
            }
            pthread_mutex_lock(&g_lock);
            snprintf(j->upload_url, sizeof j->upload_url, "%s", up_url);
            save_queue_locked();
            pthread_mutex_unlock(&g_lock);
            offset = 0;
            break;
        }
    }

    if (j->stop) {
        fclose(fp);
        memset(access_token, 0, sizeof access_token);
        pthread_mutex_lock(&g_lock);
        j->state = j->stop == 2 ? U_CANCELLED : U_PAUSED;
        j->speed = 0;
        save_queue_locked();
        g_running--;
        schedule_locked();
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    pthread_mutex_lock(&g_lock);
    j->state = U_UPLOADING;
    j->done = offset;
    j->start_done = offset;
    j->started_ms = plat_now_ms();
    pthread_mutex_unlock(&g_lock);

    int64_t chunk_bytes = (int64_t)chunk_mb * 1024 * 1024;
    int retries = 0;
    const int max_retries = 5;

    while (!already_final && offset < j->total) {
        if (j->stop) break;
        int64_t rem = j->total - offset;
        int64_t clen = rem > chunk_bytes ? chunk_bytes : rem;
        int64_t acked = offset;
        quota_exceeded = 0;

        int crc = yt_upload_chunk(access_token, up_url, fp, mime, offset, clen, j->total,
                                  on_chunk_progress, j, &j->stop,
                                  &acked, video_id, sizeof video_id, &http_status, &quota_exceeded,
                                  err, sizeof err);
        if (j->stop) break;
        if (crc != 0 && http_status == 401) {
            pthread_mutex_lock(&g_lock);
            j->done = offset;
            pthread_mutex_unlock(&g_lock);
            if (ensure_access_token(access_token, sizeof access_token, 1, &invalid_grant, err, sizeof err) == 0) {
                crc = yt_upload_chunk(access_token, up_url, fp, mime, offset, clen, j->total,
                                          on_chunk_progress, j, &j->stop,
                                          &acked, video_id, sizeof video_id, &http_status, &quota_exceeded,
                                          err, sizeof err);
                if (j->stop) break;
            } else {
                fclose(fp);
                memset(access_token, 0, sizeof access_token);
                fail_job(j, err[0] ? err : "YouTube sign-in expired — scan QR to sign in again");
                return NULL;
            }
        }
        if (crc == 0) {
            retries = 0;
            offset = acked;
            pthread_mutex_lock(&g_lock);
            j->done = offset;
            save_queue_locked();
            pthread_mutex_unlock(&g_lock);
            if (video_id[0]) break;
            continue;
        }

        if (quota_exceeded) {
            fclose(fp);
            memset(access_token, 0, sizeof access_token);
            fail_job(j, err[0] ? err : "YouTube API daily upload quota reached — resets at midnight Pacific Time");
            return NULL;
        }
        if (http_status == 401) {
            fclose(fp);
            memset(access_token, 0, sizeof access_token);
            fail_job(j, "YouTube sign-in expired — scan QR to sign in again");
            return NULL;
        }
        if (++retries > max_retries) {
            fclose(fp);
            memset(access_token, 0, sizeof access_token);
            fail_job(j, err[0] ? err : "upload interrupted — press Retry to resume");
            return NULL;
        }
        plat_log("upload #%d: chunk error (%s), querying upload offset (retry %d/%d)",
                 j->id, err[0] ? err : "network drop", retries, max_retries);
        plat_sleep_ms(200);
        if (j->stop) break;

        int64_t srv_off = offset;
        int q_final = 0;
        int qrc = yt_upload_query(access_token, up_url, j->total, &srv_off, &q_final,
                                  video_id, sizeof video_id, &http_status, err, sizeof err);
        if (qrc != 0 && http_status == 401) {
            if (ensure_access_token(access_token, sizeof access_token, 1, &invalid_grant, err, sizeof err) == 0) {
                qrc = yt_upload_query(access_token, up_url, j->total, &srv_off, &q_final,
                                      video_id, sizeof video_id, &http_status, err, sizeof err);
            } else {
                fclose(fp);
                memset(access_token, 0, sizeof access_token);
                fail_job(j, err[0] ? err : "YouTube sign-in expired — scan QR to sign in again");
                return NULL;
            }
        }
        if (http_status == 401) {
            fclose(fp);
            memset(access_token, 0, sizeof access_token);
            fail_job(j, "YouTube sign-in expired — scan QR to sign in again");
            return NULL;
        }
        if (http_status == 404 || http_status == 410) {
            if (yt_upload_init(access_token, &meta, mime, j->total, up_url, sizeof up_url,
                               &http_status, &quota_exceeded, err, sizeof err) != 0) {
                fclose(fp);
                memset(access_token, 0, sizeof access_token);
                fail_job(j, err[0] ? err : "upload session expired and re-init failed");
                return NULL;
            }
            offset = 0;
            pthread_mutex_lock(&g_lock);
            snprintf(j->upload_url, sizeof j->upload_url, "%s", up_url);
            j->done = 0;
            j->start_done = 0;
            j->started_ms = plat_now_ms();
            save_queue_locked();
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        if (qrc == 0) {
            if (q_final && video_id[0]) {
                already_final = 1;
                break;
            }
            if (srv_off >= 0 && srv_off <= j->total) {
                offset = srv_off;
                pthread_mutex_lock(&g_lock);
                j->done = offset;
                j->start_done = offset;
                j->started_ms = plat_now_ms();
                pthread_mutex_unlock(&g_lock);
            }
        }
    }

    fclose(fp);

    if (j->stop) {
        memset(access_token, 0, sizeof access_token);
        pthread_mutex_lock(&g_lock);
        j->state = j->stop == 2 ? U_CANCELLED : U_PAUSED;
        j->speed = 0;
        save_queue_locked();
        g_running--;
        schedule_locked();
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    if (!video_id[0]) {
        memset(access_token, 0, sizeof access_token);
        fail_job(j, err[0] ? err : "missing video id from YouTube");
        return NULL;
    }

    pthread_mutex_lock(&g_lock);
    j->state = U_FINALIZING;
    j->done = j->total;
    j->speed = 0;
    pthread_mutex_unlock(&g_lock);

    // Record video_id immediately so subsequent playlist failure never duplicates the upload.
    clips_history_mark(j->path, video_id, meta.title);

    char note[256] = "";
    if (!meta.made_for_kids && meta.playlist_id[0]) {
        char perr[256] = "";
        int prc = yt_playlist_add(access_token, meta.playlist_id, video_id, &http_status, perr, sizeof perr);
        if (prc != 0 && http_status == 401) {
            if (ensure_access_token(access_token, sizeof access_token, 1, &invalid_grant, perr, sizeof perr) == 0) {
                prc = yt_playlist_add(access_token, meta.playlist_id, video_id, &http_status, perr, sizeof perr);
            }
        }
        if (prc != 0) {
            plat_log("upload #%d: video %s uploaded, but playlistItems failed: %s", j->id, video_id, perr);
            snprintf(note, sizeof note, "Uploaded, but adding to playlist failed (%s) — check YouTube Studio",
                     perr[0] ? perr : "HTTP error");
        }
    }
    memset(access_token, 0, sizeof access_token);

    pthread_mutex_lock(&g_lock);
    snprintf(j->video_id, sizeof j->video_id, "%s", video_id);
    snprintf(j->note, sizeof j->note, "%s", note);
    j->err[0] = 0;
    j->state = U_DONE;
    j->finished_ms = plat_now_ms();
    save_queue_locked();
    g_running--;
    schedule_locked();
    pthread_mutex_unlock(&g_lock);

    plat_notify("YouTube: uploaded %s (https://youtu.be/%s)", meta.title[0] ? meta.title : j->name, video_id);
    return NULL;
}

static void schedule_locked(void) {
    if (g_quit) return;
    int par = g_settings.parallel > 0 ? g_settings.parallel : 1;
    for (int i = 0; i < g_njobs && g_running < par; i++) {
        upload_job *j = g_jobs[i];
        if (j->state != U_QUEUED) continue;
        j->state = U_STARTING;
        j->stop = 0;
        j->err[0] = 0;
        j->note[0] = 0;
        g_running++;
        if (plat_start_thread(upload_worker, j)) {
            g_running--;
            j->state = U_FAILED;
            snprintf(j->err, sizeof j->err, "cannot start upload worker thread");
        }
    }
}

// ---------- optional auto-upload watcher (off by default) ----------

static int watch_is_baselined_locked(const char *path) {
    for (int i = 0; i < g_nwatch; i++)
        if (!strcmp(g_watch[i].path, path) && g_watch[i].baselined) return 1;
    return 0;
}

static watch_entry *watch_find_or_add_locked(const char *path, int *added_new) {
    if (added_new) *added_new = 0;
    for (int i = 0; i < g_nwatch; i++)
        if (!strcmp(g_watch[i].path, path)) return &g_watch[i];
    if (g_nwatch == g_capwatch) {
        int nc = g_capwatch ? g_capwatch * 2 : 64;
        watch_entry *nw = realloc(g_watch, (size_t)nc * sizeof *nw);
        if (!nw) return NULL;
        g_watch = nw;
        g_capwatch = nc;
    }
    watch_entry *w = &g_watch[g_nwatch++];
    memset(w, 0, sizeof *w);
    snprintf(w->path, sizeof w->path, "%s", path);
    if (added_new) *added_new = 1;
    return w;
}

static void snapshot_existing_clips_locked(const char *dir) {
    clip_entry *arr = NULL;
    int n = 0;
    if (clips_scan_array(dir, &arr, &n) != 0) return;
    for (int i = 0; i < n; i++) {
        watch_entry *w = watch_find_or_add_locked(arr[i].path, NULL);
        if (w) {
            w->baselined = 1;
            w->last_size = arr[i].size;
        }
    }
    free(arr);
}

static int path_in_queue_locked(const char *path) {
    for (int i = 0; i < g_njobs; i++) {
        upload_job *j = g_jobs[i];
        if (!strcmp(j->path, path) && j->state != U_DONE && j->state != U_CANCELLED && j->state != U_FAILED)
            return 1;
    }
    return 0;
}

static void watcher_scan_once(void) {
    char dir[512], privacy[16];
    int64_t since;
    int mfk;
    pthread_mutex_lock(&g_lock);
    if (!g_settings.auto_upload || g_quit) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(dir, sizeof dir, "%s", g_settings.captures_dir);
    snprintf(privacy, sizeof privacy, "%s", g_settings.default_privacy);
    since = g_settings.auto_upload_since;
    mfk = g_settings.made_for_kids;
    pthread_mutex_unlock(&g_lock);

    if (!net_oauth_is_logged_in()) return;

    clip_entry *arr = NULL;
    int n = 0;
    if (clips_scan_array(dir, &arr, &n) != 0 || !arr || n == 0) {
        free(arr);
        return;
    }

    typedef struct {
        char path[1024];
        char title[256];
    } to_queue;
    to_queue *qlist = NULL;
    int nq = 0, capq = 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < n; i++) {
        const clip_entry *c = &arr[i];
        if (c->size <= 0) continue;
        if (since > 0 && c->mtime < since) continue;
        if (watch_is_baselined_locked(c->path)) continue;
        if (clips_history_lookup(c->path, NULL, 0) || path_in_queue_locked(c->path)) {
            watch_entry *w = watch_find_or_add_locked(c->path, NULL);
            if (w) w->baselined = 1;
            continue;
        }
        int added = 0;
        watch_entry *w = watch_find_or_add_locked(c->path, &added);
        if (!w) continue;
        if (added || c->size != w->last_size) {
            w->last_size = c->size;
            w->stable_scans = 0;
            continue;
        }
        w->stable_scans++;
        if (w->stable_scans >= 1) {
            w->baselined = 1;
            if (nq == capq) {
                int nc = capq ? capq * 2 : 8;
                to_queue *nl = realloc(qlist, (size_t)nc * sizeof *nl);
                if (!nl) continue;
                qlist = nl;
                capq = nc;
            }
            snprintf(qlist[nq].path, sizeof qlist[nq].path, "%s", c->path);
            if (c->title_id[0]) snprintf(qlist[nq].title, sizeof qlist[nq].title, "PS5 Capture - %s", c->title_id);
            else snprintf(qlist[nq].title, sizeof qlist[nq].title, "%s", c->name);
            nq++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(arr);

    for (int i = 0; i < nq; i++) {
        yt_video_meta meta;
        memset(&meta, 0, sizeof meta);
        snprintf(meta.title, sizeof meta.title, "%s", qlist[i].title);
        snprintf(meta.description, sizeof meta.description, "Uploaded from PlayStation 5 via PS5 YouTube Uploader");
        snprintf(meta.privacy, sizeof meta.privacy, "%s", privacy);
        meta.made_for_kids = mfk;
        char err[256] = "";
        upload_add(qlist[i].path, &meta, err, sizeof err);
    }
    free(qlist);
}

static void *watcher_thread(void *arg) {
    (void)arg;
    const char *env = getenv("PS5YT_WATCH_MS");
    int watch_ms = (env && atoi(env) >= 100) ? atoi(env) : 10000;

    pthread_mutex_lock(&g_lock);
    while (!g_quit) {
        if (!g_settings.auto_upload) {
            pthread_cond_wait(&g_cv, &g_lock);
            continue;
        }
        pthread_mutex_unlock(&g_lock);
        watcher_scan_once();
        pthread_mutex_lock(&g_lock);
        if (g_quit) break;
        if (!g_settings.auto_upload) continue;

        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct timespec ts;
        ts.tv_sec = tv.tv_sec + (watch_ms / 1000);
        long nsec = (tv.tv_usec * 1000L) + (long)(watch_ms % 1000) * 1000000L;
        ts.tv_sec += nsec / 1000000000L;
        ts.tv_nsec = nsec % 1000000000L;
        pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }
    g_watcher_alive = 0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// ---------- public queue & settings API ----------

void upload_init(void) {
    pthread_mutex_lock(&g_lock);
    g_quit = 0;
    load_settings();
    load_queue();
    schedule_locked();
    if (!g_watcher_alive) {
        g_watcher_alive = 1;
        if (plat_start_thread(watcher_thread, NULL) != 0) g_watcher_alive = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void upload_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    g_quit = 1;
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i]->state == U_STARTING || g_jobs[i]->state == U_UPLOADING)
            g_jobs[i]->stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&g_lock);
        int busy = g_running > 0 || g_watcher_alive;
        pthread_mutex_unlock(&g_lock);
        if (!busy) break;
        plat_sleep_ms(100);
    }
    pthread_mutex_lock(&g_lock);
    save_queue_locked();
    pthread_mutex_unlock(&g_lock);
}

void upload_settings_get(app_settings *out) {
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_settings;
    pthread_mutex_unlock(&g_lock);
}

int upload_settings_set(const app_settings *in, char *err, size_t errlen) {
    if (!in) return -1;
    if (strcmp(in->default_privacy, "UNLISTED") &&
        strcmp(in->default_privacy, "PUBLIC") &&
        strcmp(in->default_privacy, "PRIVATE")) {
        if (err && errlen) snprintf(err, errlen, "privacy must be UNLISTED, PUBLIC or PRIVATE");
        return -1;
    }
    if (in->chunk_mb != 4 && in->chunk_mb != 8 && in->chunk_mb != 16) {
        if (err && errlen) snprintf(err, errlen, "chunk size must be 4, 8 or 16 MB");
        return -1;
    }
    if (in->parallel < 1 || in->parallel > 2) {
        if (err && errlen) snprintf(err, errlen, "parallel uploads must be 1 or 2");
        return -1;
    }
    if (in->captures_dir[0] && !plat_path_allowed(in->captures_dir)) {
        if (err && errlen) snprintf(err, errlen, "captures folder is outside allowed paths");
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    int prev_auto = g_settings.auto_upload;
    snprintf(g_settings.default_privacy, sizeof g_settings.default_privacy, "%s", in->default_privacy);
    g_settings.made_for_kids = in->made_for_kids ? 1 : 0;
    g_settings.chunk_mb = in->chunk_mb;
    g_settings.parallel = in->parallel;
    if (in->captures_dir[0]) snprintf(g_settings.captures_dir, sizeof g_settings.captures_dir, "%s", in->captures_dir);

    if (!prev_auto && in->auto_upload) {
        g_settings.auto_upload = 1;
        g_settings.auto_upload_since = (int64_t)time(NULL);
        snapshot_existing_clips_locked(g_settings.captures_dir);
    } else if (!in->auto_upload) {
        g_settings.auto_upload = 0;
        g_settings.auto_upload_since = 0;
    }
    save_settings_locked();
    schedule_locked();
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int upload_add(const char *path, const yt_video_meta *meta, char *err, size_t errlen) {
    if (!path || !path[0] || !plat_path_allowed(path) || !is_video_filename(path)) {
        if (err && errlen) snprintf(err, errlen, "invalid capture file path");
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (err && errlen) snprintf(err, errlen, "capture file does not exist or is empty");
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (path_in_queue_locked(path)) {
        pthread_mutex_unlock(&g_lock);
        if (err && errlen) snprintf(err, errlen, "this clip is already in the queue");
        return -1;
    }
    upload_job *j = new_job_locked();
    if (!j) {
        pthread_mutex_unlock(&g_lock);
        if (err && errlen) snprintf(err, errlen, "upload queue is full");
        return -1;
    }
    j->state = U_QUEUED;
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->name, sizeof j->name, "%s", base_name(path));
    j->total = (int64_t)st.st_size;

    if (meta) j->meta = *meta;
    if (!j->meta.title[0]) snprintf(j->meta.title, sizeof j->meta.title, "%s", j->name);
    if (!j->meta.privacy[0]) snprintf(j->meta.privacy, sizeof j->meta.privacy, "%s", g_settings.default_privacy);

    int id = j->id;
    save_queue_locked();
    schedule_locked();
    pthread_mutex_unlock(&g_lock);
    return id;
}

void upload_jobs_json(sbuf *out) {
    sb_init(out);
    pthread_mutex_lock(&g_lock);
    sb_printf(out, "{\"ok\":true,\"jobs\":[");
    for (int i = 0; i < g_njobs; i++) {
        upload_job *j = g_jobs[i];
        if (i) sb_puts(out, ",");
        sb_printf(out, "{\"id\":%d,\"state\":\"%s\",\"path\":", j->id, state_names[j->state]);
        sb_json_str(out, j->path);
        sb_puts(out, ",\"name\":");
        sb_json_str(out, j->name);
        sb_puts(out, ",\"title\":");
        sb_json_str(out, j->meta.title);
        sb_puts(out, ",\"privacy\":");
        sb_json_str(out, j->meta.privacy);
        sb_printf(out, ",\"made_for_kids\":%s,\"done\":%lld,\"total\":%lld,\"speed\":%.0f,\"video_id\":",
                  j->meta.made_for_kids ? "true" : "false",
                  (long long)j->done, (long long)j->total, j->speed);
        sb_json_str(out, j->video_id);
        sb_puts(out, ",\"url\":");
        if (j->video_id[0]) {
            char yurl[96];
            snprintf(yurl, sizeof yurl, "https://youtu.be/%s", j->video_id);
            sb_json_str(out, yurl);
        } else {
            sb_puts(out, "null");
        }
        sb_puts(out, ",\"err\":");
        sb_json_str(out, j->err);
        sb_puts(out, ",\"note\":");
        sb_json_str(out, j->note);
        sb_puts(out, "}");
    }
    sb_puts(out, "]}");
    pthread_mutex_unlock(&g_lock);
}

static upload_job *find_job_locked(int id) {
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i]->id == id) return g_jobs[i];
    return NULL;
}

int upload_pause(int id) {
    pthread_mutex_lock(&g_lock);
    upload_job *j = find_job_locked(id);
    int rc = -1;
    if (j && (j->state == U_QUEUED || j->state == U_STARTING || j->state == U_UPLOADING)) {
        if (j->state == U_QUEUED) j->state = U_PAUSED, save_queue_locked();
        else j->stop = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int upload_resume(int id) {
    pthread_mutex_lock(&g_lock);
    upload_job *j = find_job_locked(id);
    int rc = -1;
    if (j && (j->state == U_PAUSED || j->state == U_FAILED || j->state == U_CANCELLED)) {
        j->state = U_QUEUED;
        j->stop = 0;
        j->err[0] = 0;
        j->note[0] = 0;
        save_queue_locked();
        schedule_locked();
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int upload_cancel(int id) {
    pthread_mutex_lock(&g_lock);
    upload_job *j = find_job_locked(id);
    int rc = -1;
    if (j && (j->state == U_QUEUED || j->state == U_STARTING || j->state == U_UPLOADING ||
              j->state == U_PAUSED || j->state == U_FAILED)) {
        if (j->state == U_STARTING || j->state == U_UPLOADING) j->stop = 2;
        else j->state = U_CANCELLED, save_queue_locked();
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int upload_remove(int id) {
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    for (int i = 0; i < g_njobs; i++) {
        upload_job *j = g_jobs[i];
        if (j->id != id) continue;
        if (j->state != U_DONE && j->state != U_FAILED && j->state != U_CANCELLED && j->state != U_PAUSED) break;
        free(j);
        memmove(g_jobs + i, g_jobs + i + 1, (size_t)(g_njobs - i - 1) * sizeof *g_jobs);
        g_njobs--;
        save_queue_locked();
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void upload_clear_finished(void) {
    pthread_mutex_lock(&g_lock);
    int out = 0;
    for (int i = 0; i < g_njobs; i++) {
        if (g_jobs[i]->state == U_DONE || g_jobs[i]->state == U_CANCELLED) free(g_jobs[i]);
        else g_jobs[out++] = g_jobs[i];
    }
    g_njobs = out;
    save_queue_locked();
    pthread_mutex_unlock(&g_lock);
}
