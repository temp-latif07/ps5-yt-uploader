// PS5 capture scanner, folder/USB browser, and uploaded clip history.
#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

typedef struct {
    char path[1024];
    char video_id[64];
    int64_t ts;
    char title[256];
} hist_entry;

static pthread_mutex_t g_hist_lock = PTHREAD_MUTEX_INITIALIZER;
static hist_entry *g_hist;
static int g_nhist, g_caphist;

static void tsv_sanitize(char *dst, size_t len, const char *src) {
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 1 < len; p++)
        dst[o++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    if (len) dst[o] = 0;
}

static void history_load_locked(void) {
    free(g_hist);
    g_hist = NULL;
    g_nhist = g_caphist = 0;

    char p[600];
    snprintf(p, sizeof p, "%s/uploaded.tsv", plat_app_dir());
    FILE *f = fopen(p, "r");
    if (!f) return;

    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *vid = t1 + 1;
        char *t2 = strchr(vid, '\t');
        int64_t ts = 0;
        const char *title = "";
        if (t2) {
            *t2 = 0;
            char *ts_s = t2 + 1;
            char *t3 = strchr(ts_s, '\t');
            if (t3) {
                *t3 = 0;
                title = t3 + 1;
            }
            ts = (int64_t)strtoll(ts_s, NULL, 10);
        }
        if (g_nhist == g_caphist) {
            int nc = g_caphist ? g_caphist * 2 : 64;
            hist_entry *nh = realloc(g_hist, (size_t)nc * sizeof *nh);
            if (!nh) break;
            g_hist = nh;
            g_caphist = nc;
        }
        hist_entry *e = &g_hist[g_nhist++];
        snprintf(e->path, sizeof e->path, "%s", line);
        snprintf(e->video_id, sizeof e->video_id, "%s", vid);
        e->ts = ts;
        snprintf(e->title, sizeof e->title, "%s", title);
    }
    fclose(f);
}

static void history_save_locked(void) {
    char p[600], tmp[620];
    snprintf(p, sizeof p, "%s/uploaded.tsv", plat_app_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_nhist; i++) {
        fprintf(f, "%s\t%s\t%lld\t%s\n", g_hist[i].path, g_hist[i].video_id,
                (long long)g_hist[i].ts, g_hist[i].title);
    }
    fclose(f);
    rename(tmp, p);
}

void clips_init(void) {
    pthread_mutex_lock(&g_hist_lock);
    history_load_locked();
    pthread_mutex_unlock(&g_hist_lock);
}

int clips_history_lookup(const char *path, char *vid_out, size_t vid_len) {
    if (vid_out && vid_len) vid_out[0] = 0;
    if (!path || !path[0]) return 0;
    pthread_mutex_lock(&g_hist_lock);
    int found = 0;
    for (int i = g_nhist - 1; i >= 0; i--) {
        if (!strcmp(g_hist[i].path, path)) {
            if (vid_out && vid_len) snprintf(vid_out, vid_len, "%s", g_hist[i].video_id);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_hist_lock);
    return found;
}

int clips_history_mark(const char *path, const char *video_id, const char *title) {
    if (!path || !path[0] || !video_id || !video_id[0]) return -1;
    pthread_mutex_lock(&g_hist_lock);
    int idx = -1;
    for (int i = 0; i < g_nhist; i++) {
        if (!strcmp(g_hist[i].path, path)) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_nhist == g_caphist) {
            int nc = g_caphist ? g_caphist * 2 : 64;
            hist_entry *nh = realloc(g_hist, (size_t)nc * sizeof *nh);
            if (!nh) {
                pthread_mutex_unlock(&g_hist_lock);
                return -1;
            }
            g_hist = nh;
            g_caphist = nc;
        }
        idx = g_nhist++;
    }
    tsv_sanitize(g_hist[idx].path, sizeof g_hist[idx].path, path);
    tsv_sanitize(g_hist[idx].video_id, sizeof g_hist[idx].video_id, video_id);
    g_hist[idx].ts = (int64_t)time(NULL);
    tsv_sanitize(g_hist[idx].title, sizeof g_hist[idx].title, title);
    history_save_locked();
    pthread_mutex_unlock(&g_hist_lock);
    return 0;
}

static int cmp_clips_desc(const void *a, const void *b) {
    const clip_entry *ca = a, *cb = b;
    if (ca->mtime > cb->mtime) return -1;
    if (ca->mtime < cb->mtime) return 1;
    return strcmp(ca->path, cb->path);
}

static int push_clip(clip_entry **arr, int *n, int *cap, const char *full, const char *name,
                     const char *title_id, const struct stat *st) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        clip_entry *na = realloc(*arr, (size_t)nc * sizeof *na);
        if (!na) return -1;
        *arr = na;
        *cap = nc;
    }
    clip_entry *c = &(*arr)[(*n)++];
    snprintf(c->path, sizeof c->path, "%s", full);
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->title_id, sizeof c->title_id, "%s", title_id ? title_id : "");
    c->size = (int64_t)st->st_size;
    c->mtime = (int64_t)st->st_mtime;
    return 0;
}

int clips_scan_array(const char *dir, clip_entry **out, int *count_out) {
    *out = NULL;
    *count_out = 0;
    if (!dir || !dir[0] || !plat_path_allowed(dir)) return -1;

    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;

    clip_entry *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            DIR *sd = opendir(sub);
            if (!sd) continue;
            struct dirent *se;
            while ((se = readdir(sd))) {
                if (se->d_name[0] == '.' || !is_video_filename(se->d_name)) continue;
                char full[1024];
                snprintf(full, sizeof full, "%s/%s", sub, se->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode))
                    push_clip(&arr, &n, &cap, full, se->d_name, e->d_name, &fst);
            }
            closedir(sd);
        } else if (S_ISREG(st.st_mode) && is_video_filename(e->d_name)) {
            push_clip(&arr, &n, &cap, sub, e->d_name, "", &st);
        }
    }
    closedir(d);

    if (n > 1) qsort(arr, (size_t)n, sizeof *arr, cmp_clips_desc);
    *out = arr;
    *count_out = n;
    return 0;
}

int clips_scan_json(const char *dir, sbuf *out, char *err, size_t errlen) {
    if (!dir || !dir[0] || !plat_path_allowed(dir)) {
        if (err && errlen) snprintf(err, errlen, "folder is outside allowed capture/storage paths");
        return -1;
    }
    clip_entry *arr = NULL;
    int n = 0;
    if (clips_scan_array(dir, &arr, &n) != 0) {
        if (err && errlen) snprintf(err, errlen, "cannot scan %s: %s", dir, strerror(errno));
        return -1;
    }

    sb_init(out);
    sb_puts(out, "{\"ok\":true,\"dir\":");
    sb_json_str(out, dir);
    sb_puts(out, ",\"clips\":[");
    int limit = n > 200 ? 200 : n;
    for (int i = 0; i < limit; i++) {
        char vid[64] = "";
        int up = clips_history_lookup(arr[i].path, vid, sizeof vid);
        if (i) sb_puts(out, ",");
        sb_puts(out, "{\"path\":");
        sb_json_str(out, arr[i].path);
        sb_puts(out, ",\"name\":");
        sb_json_str(out, arr[i].name);
        sb_puts(out, ",\"title_id\":");
        sb_json_str(out, arr[i].title_id);
        sb_printf(out, ",\"size\":%lld,\"mtime\":%lld,\"video_id\":", (long long)arr[i].size, (long long)arr[i].mtime);
        if (up && vid[0]) sb_json_str(out, vid);
        else sb_puts(out, "null");
        sb_puts(out, "}");
    }
    sb_puts(out, "]}");
    free(arr);
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void add_roots_json(sbuf *b) {
    sb_puts(b, "\"roots\":[");
    int first = 1;
#ifdef __PROSPERO__
    sb_puts(b, "\"/user/av_content/video\",\"/data\"");
    first = 0;
    DIR *d = opendir("/mnt");
    struct stat mnt;
    if (d && stat("/mnt", &mnt)) { closedir(d); d = NULL; }
    if (d) {
        struct dirent *e;
        char *names[64];
        int n = 0;
        while ((e = readdir(d)) && n < 64) {
            char p[300];
            snprintf(p, sizeof p, "/mnt/%s", e->d_name);
            struct stat st;
            if (plat_path_allowed(p) && !stat(p, &st) && S_ISDIR(st.st_mode) && st.st_dev != mnt.st_dev)
                names[n++] = strdup(p);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof *names, cmp_str);
        for (int i = 0; i < n; i++) {
            sb_puts(b, ",");
            sb_json_str(b, names[i]);
            free(names[i]);
        }
    }
#else
    const char *cap = plat_captures_dir();
    if (cap && cap[0]) {
        sb_json_str(b, cap);
        first = 0;
    }
    const char *roots = getenv("PS5YT_ROOTS");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", roots && roots[0] ? roots : "/tmp");
    for (char *save = NULL, *t = strtok_r(buf, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
        if (cap && !strcmp(t, cap)) continue;
        if (!first) sb_puts(b, ",");
        sb_json_str(b, t);
        first = 0;
    }
#endif
    (void)first;
    sb_puts(b, "]");
}

int clips_list_dirs_json(const char *path, sbuf *out, char *err, size_t errlen) {
    char pbuf[1024] = "";
    if (path && path[0]) {
        snprintf(pbuf, sizeof pbuf, "%s", path);
        size_t n = strlen(pbuf);
        while (n > 1 && pbuf[n - 1] == '/') pbuf[--n] = 0;
        if (!plat_path_allowed(pbuf)) {
            if (err && errlen) snprintf(err, errlen, "this folder is outside allowed capture/storage paths");
            return -1;
        }
    }

    sb_init(out);
    sb_puts(out, "{\"ok\":true,");
    add_roots_json(out);

    if (pbuf[0]) {
        DIR *d = opendir(pbuf);
        if (!d) {
            sb_free(out);
            if (err && errlen) snprintf(err, errlen, "cannot open %s: %s", pbuf, strerror(errno));
            return -1;
        }
        char **names = NULL;
        int count = 0, cap = 0, nfiles = 0;
        sbuf files;
        sb_init(&files);
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char full[1300];
            snprintf(full, sizeof full, "%s/%s", pbuf, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            if (!S_ISDIR(st.st_mode)) {
                if (nfiles < 2000 && is_video_filename(e->d_name) && S_ISREG(st.st_mode)) {
                    if (nfiles++) sb_puts(&files, ",");
                    sb_puts(&files, "{\"name\":");
                    sb_json_str(&files, e->d_name);
                    sb_printf(&files, ",\"size\":%lld}", (long long)st.st_size);
                }
                continue;
            }
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                char **nn = realloc(names, (size_t)cap * sizeof *names);
                if (!nn) break;
                names = nn;
            }
            names[count++] = strdup(e->d_name);
            if (count >= 5000) break;
        }
        closedir(d);
        if (count > 1) qsort(names, (size_t)count, sizeof *names, cmp_str);

        sb_puts(out, ",\"path\":");
        sb_json_str(out, pbuf);

        char parent[1024];
        snprintf(parent, sizeof parent, "%s", pbuf);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = 0;
        else snprintf(parent, sizeof parent, "/");
        sb_puts(out, ",\"parent\":");
        if (plat_path_allowed(parent) && strcmp(parent, pbuf)) sb_json_str(out, parent);
        else sb_puts(out, "null");

        struct statvfs vs;
        if (!statvfs(pbuf, &vs))
            sb_printf(out, ",\"free\":%llu", (unsigned long long)vs.f_bavail * vs.f_frsize);

        sb_puts(out, ",\"dirs\":[");
        for (int i = 0; i < count; i++) {
            if (i) sb_puts(out, ",");
            sb_json_str(out, names[i]);
            free(names[i]);
        }
        free(names);
        sb_printf(out, "],\"files\":[%s]", files.data ? files.data : "");
        sb_free(&files);
    }
    sb_puts(out, "}");
    return 0;
}
