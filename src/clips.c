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
                     const char *title_id, const char *game_name, const struct stat *st) {
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
    snprintf(c->game_name, sizeof c->game_name, "%s", game_name ? game_name : "");
    c->size = (int64_t)st->st_size;
    c->mtime = (int64_t)st->st_mtime;
    return 0;
}

typedef struct {
    char tid[64];
    char name[160];
} title_map_entry;

static int read_dat_game_name(const char *dat_path, char *out, size_t len) {
    if (!dat_path || !out || !len) return 0;
    out[0] = 0;
    FILE *f = fopen(dat_path, "rb");
    if (!f) return 0;
    char raw[256];
    memset(raw, 0, sizeof raw);
    int ok = (fseek(f, 0x44L, SEEK_SET) == 0 && fread(raw, 1, sizeof raw - 1, f) > 0);
    fclose(f);
    if (!ok || !raw[0]) return 0;
    raw[sizeof raw - 1] = 0;
    for (unsigned char *p = (unsigned char *)raw; *p; p++)
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
    str_trim(raw);
    char *us = strrchr(raw, '_');
    if (us && strlen(us + 1) == 14) {
        int digits = 1;
        for (const char *q = us + 1; *q; q++)
            if (*q < '0' || *q > '9') { digits = 0; break; }
        if (digits) *us = 0;
    }
    str_trim(raw);
    if (!raw[0]) return 0;
    snprintf(out, len, "%s", raw);
    return 1;
}

#ifdef __PROSPERO__
static int read_appmeta_game_name(const char *tid, char *out, size_t len) {
    if (!tid || !tid[0] || !out || !len) return 0;
    out[0] = 0;
    char p[256];
    snprintf(p, sizeof p, "/user/appmeta/%s/param.json", tid);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char *buf = malloc(16384);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t n = fread(buf, 1, 16383, f);
    fclose(f);
    buf[n] = 0;
    const char *v = json_find(buf, "titleName");
    if (!v) {
        const char *lp = json_find(buf, "localizedParameters");
        const char *en = json_find(lp, "en-US");
        v = json_find(en, "titleName");
    }
    int ok = (v && json_string(v, out, len) == 0 && out[0] != 0);
    free(buf);
    return ok;
}
#endif

static const char *title_cache_lookup(const title_map_entry *map, int nmap, const char *tid) {
    if (!tid || !tid[0]) return NULL;
    for (int i = 0; i < nmap; i++)
        if (!strcmp(map[i].tid, tid)) return map[i].name;
    return NULL;
}

static void title_cache_put(title_map_entry *map, int *nmap, int max_map, const char *tid, const char *name) {
    if (!tid || !tid[0] || !name || !name[0] || *nmap >= max_map) return;
    if (title_cache_lookup(map, *nmap, tid)) return;
    snprintf(map[*nmap].tid, sizeof map[*nmap].tid, "%s", tid);
    snprintf(map[*nmap].name, sizeof map[*nmap].name, "%s", name);
    (*nmap)++;
}

static int is_game_title_id(const char *name) {
    if (!name || strlen(name) != 9 || !strcmp(name, "NPXS40087")) return 0;
    for (int i = 0; i < 4; i++)
        if (name[i] < 'A' || name[i] > 'Z') return 0;
    for (int i = 4; i < 9; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}

static void resolve_clip_game_name(const char *dir, const char *clip_name, const char *tid,
                                   const char *dir_dat_name, title_map_entry *map, int *nmap, int max_map,
                                   char *out, size_t len) {
    out[0] = 0;
    const char *dot = strrchr(clip_name, '.');
    if (dot && dot > clip_name) {
        size_t stem = (size_t)(dot - clip_name);
        if (stem + 5 < 256) {
            char dat_path[1280];
            snprintf(dat_path, sizeof dat_path, "%s/%.*s.dat", dir, (int)stem, clip_name);
            if (read_dat_game_name(dat_path, out, len)) {
                title_cache_put(map, nmap, max_map, tid, out);
                return;
            }
        }
    }
    if (dir_dat_name && dir_dat_name[0]) {
        snprintf(out, len, "%s", dir_dat_name);
        title_cache_put(map, nmap, max_map, tid, out);
        return;
    }
    const char *cached = title_cache_lookup(map, *nmap, tid);
    if (cached) {
        snprintf(out, len, "%s", cached);
        return;
    }
#ifdef __PROSPERO__
    if (tid && tid[0] && read_appmeta_game_name(tid, out, len)) {
        title_cache_put(map, nmap, max_map, tid, out);
        return;
    }
#endif
}

static void scan_dir_rec(const char *dir, int depth, const char *inherited_tid,
                         clip_entry **arr, int *n, int *cap,
                         title_map_entry *map, int *nmap, int max_map) {
    if (depth > 5 || *n >= 2000) return;
    DIR *d = opendir(dir);
    if (!d) return;

    char dir_dat_name[160] = "";
    int start_idx = *n;
    struct dirent *e;
    while ((e = readdir(d)) && *n < 2000) {
        if (e->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            const char *next_tid = inherited_tid;
            if (is_game_title_id(e->d_name)) next_tid = e->d_name;
            else if (!next_tid[0] &&
                     strcasecmp(e->d_name, "NPXS40087") != 0 &&
                     strcasecmp(e->d_name, "video") != 0 &&
                     strcasecmp(e->d_name, "av_contents") != 0 &&
                     strcasecmp(e->d_name, "av_content") != 0) {
                next_tid = e->d_name;
            }
            scan_dir_rec(sub, depth + 1, next_tid, arr, n, cap, map, nmap, max_map);
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(e->d_name);
            if (!dir_dat_name[0] && nl > 4 && !strcasecmp(e->d_name + nl - 4, ".dat")) {
                read_dat_game_name(sub, dir_dat_name, sizeof dir_dat_name);
                title_cache_put(map, nmap, max_map, inherited_tid, dir_dat_name);
            } else if (is_video_filename(e->d_name)) {
                char gname[160];
                resolve_clip_game_name(dir, e->d_name, inherited_tid, dir_dat_name,
                                       map, nmap, max_map, gname, sizeof gname);
                push_clip(arr, n, cap, sub, e->d_name, inherited_tid, gname, &st);
            }
        }
    }
    closedir(d);

    for (int i = start_idx; i < *n; i++) {
        if (!(*arr)[i].game_name[0]) {
            if (dir_dat_name[0] && !strncmp((*arr)[i].path, dir, strlen(dir))) {
                snprintf((*arr)[i].game_name, sizeof (*arr)[i].game_name, "%s", dir_dat_name);
            } else {
                const char *cached = title_cache_lookup(map, *nmap, (*arr)[i].title_id);
                if (cached) snprintf((*arr)[i].game_name, sizeof (*arr)[i].game_name, "%s", cached);
            }
        }
    }
}

int clips_scan_array(const char *dir, clip_entry **out, int *count_out) {
    *out = NULL;
    *count_out = 0;
    if (!dir || !dir[0] || !plat_path_allowed(dir)) return -1;

    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    closedir(d);

    title_map_entry *map = calloc(128, sizeof *map);
    int nmap = 0;
    clip_entry *arr = NULL;
    int n = 0, cap = 0;
    scan_dir_rec(dir, 0, "", &arr, &n, &cap, map, &nmap, map ? 128 : 0);
    free(map);

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
        sb_puts(out, ",\"game_name\":");
        sb_json_str(out, arr[i].game_name);
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
    struct stat rst;
    if (stat("/user/av_contents/video", &rst) == 0 && S_ISDIR(rst.st_mode)) {
        sb_puts(b, "\"/user/av_contents/video\"");
        first = 0;
    } else if (stat("/user/av_contents", &rst) == 0 && S_ISDIR(rst.st_mode)) {
        sb_puts(b, "\"/user/av_contents\"");
        first = 0;
    }
    if (stat("/user/av_content/video", &rst) == 0 && S_ISDIR(rst.st_mode)) {
        if (!first) sb_puts(b, ",");
        sb_puts(b, "\"/user/av_content/video\"");
        first = 0;
    }
    if (!first) sb_puts(b, ",");
    sb_puts(b, "\"/data\"");
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
