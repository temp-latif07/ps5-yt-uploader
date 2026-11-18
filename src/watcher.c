#define _POSIX_C_SOURCE 200809L
#include "watcher.h"
#include "notify.h"
#include "config.h"
#include "youtube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>

typedef struct watched_file_node {
    char filepath[512];
    char title_id[32];
    off_t last_size;
    time_t last_mtime;
    int stable_count;       /* Consecutive scans where size > 0 and unchanged */
    int dispatched;         /* 1 if callback already triggered */
    int seen_in_scan;       /* Marked during scan; 0 indicates deleted file */
    struct watched_file_node* next;
} watched_file_node_t;

static char s_captures_root[512] = {0};
static watcher_clip_ready_cb s_cb = NULL;
static void* s_userdata = NULL;
static int s_poll_interval_ms = 2000;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_thread;
static int s_thread_running = 0;
static int s_thread_spawned = 0;

static watched_file_node_t* s_tracked_files = NULL;

static int is_video_clip_file(const char* name) {
    if (!name || name[0] == '.') {
        return 0;
    }
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".mp4") == 0) {
        return 1;
    }
    if (len >= 5 && strcasecmp(name + len - 5, ".webm") == 0) {
        return 1;
    }
    return 0;
}

static int compare_clips_desc(const void* a, const void* b) {
    const capture_clip_t* ca = (const capture_clip_t*)a;
    const capture_clip_t* cb = (const capture_clip_t*)b;
    if (ca->timestamp > cb->timestamp) return -1;
    if (ca->timestamp < cb->timestamp) return 1;
    return strcmp(ca->filepath, cb->filepath);
}

int scan_existing_clips(const char* captures_root, capture_clip_t** clips_out, size_t* count_out) {
    if (!captures_root || !clips_out || !count_out) {
        return -1;
    }

    *clips_out = NULL;
    *count_out = 0;

    DIR* root_dir = opendir(captures_root);
    if (!root_dir) {
        return -1;
    }

    size_t capacity = 16;
    size_t count = 0;
    capture_clip_t* clips = malloc(capacity * sizeof(capture_clip_t));
    if (!clips) {
        closedir(root_dir);
        return -1;
    }

    struct dirent* entry;
    while ((entry = readdir(root_dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char subpath[1024];
        snprintf(subpath, sizeof(subpath), "%s/%s", captures_root, entry->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            DIR* subdir = opendir(subpath);
            if (!subdir) {
                continue;
            }
            struct dirent* sub_entry;
            while ((sub_entry = readdir(subdir)) != NULL) {
                if (sub_entry->d_name[0] == '.' || !is_video_clip_file(sub_entry->d_name)) {
                    continue;
                }
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", subpath, sub_entry->d_name);

                struct stat fst;
                if (stat(filepath, &fst) != 0 || !S_ISREG(fst.st_mode)) {
                    continue;
                }

                if (count >= capacity) {
                    size_t new_cap = capacity * 2;
                    capture_clip_t* new_clips = realloc(clips, new_cap * sizeof(capture_clip_t));
                    if (!new_clips) {
                        free(clips);
                        closedir(subdir);
                        closedir(root_dir);
                        return -1;
                    }
                    clips = new_clips;
                    capacity = new_cap;
                }

                snprintf(clips[count].filepath, sizeof(clips[count].filepath), "%s", filepath);
                snprintf(clips[count].title_id, sizeof(clips[count].title_id), "%.31s", entry->d_name);
                clips[count].timestamp = fst.st_mtime;
                clips[count].size_bytes = fst.st_size;
                count++;
            }
            closedir(subdir);
        } else if (S_ISREG(st.st_mode)) {
            if (!is_video_clip_file(entry->d_name)) {
                continue;
            }

            if (count >= capacity) {
                size_t new_cap = capacity * 2;
                capture_clip_t* new_clips = realloc(clips, new_cap * sizeof(capture_clip_t));
                if (!new_clips) {
                    free(clips);
                    closedir(root_dir);
                    return -1;
                }
                clips = new_clips;
                capacity = new_cap;
            }

            snprintf(clips[count].filepath, sizeof(clips[count].filepath), "%s", subpath);
            char root_buf[512];
            snprintf(root_buf, sizeof(root_buf), "%s", captures_root);
            size_t rlen = strlen(root_buf);
            while (rlen > 1 && root_buf[rlen - 1] == '/') {
                root_buf[--rlen] = '\0';
            }
            const char* slash = strrchr(root_buf, '/');
            const char* bname = slash ? slash + 1 : root_buf;
            if (bname[0] && strcasecmp(bname, "video") != 0 && strcasecmp(bname, "av_content") != 0) {
                snprintf(clips[count].title_id, sizeof(clips[count].title_id), "%.31s", bname);
            } else {
                clips[count].title_id[0] = '\0';
            }
            clips[count].timestamp = st.st_mtime;
            clips[count].size_bytes = st.st_size;
            count++;
        }
    }
    closedir(root_dir);

    if (count == 0) {
        free(clips);
        *clips_out = NULL;
        *count_out = 0;
        return 0;
    }

    qsort(clips, count, sizeof(capture_clip_t), compare_clips_desc);
    *clips_out = clips;
    *count_out = count;
    return 0;
}

void watcher_set_poll_interval_ms(int interval_ms) {
    pthread_mutex_lock(&s_state_mutex);
    if (interval_ms > 0) {
        s_poll_interval_ms = interval_ms;
    } else {
        s_poll_interval_ms = 2000;
    }
    pthread_mutex_unlock(&s_state_mutex);
}

int watcher_init(const char* captures_root, watcher_clip_ready_cb cb, void* userdata) {
    if (!captures_root || !*captures_root) {
        return -1;
    }

    watcher_stop();

    pthread_mutex_lock(&s_state_mutex);
    snprintf(s_captures_root, sizeof(s_captures_root), "%s", captures_root);
    s_cb = cb;
    s_userdata = userdata;

    watched_file_node_t* curr = s_tracked_files;
    while (curr) {
        watched_file_node_t* next = curr->next;
        free(curr);
        curr = next;
    }
    s_tracked_files = NULL;

    pthread_mutex_unlock(&s_state_mutex);
    return 0;
}

int watcher_process_clip_upload(const capture_clip_t* clip) {
    if (!clip || !clip->filepath[0]) {
        return -1;
    }

    app_config_t cfg;
    if (config_load(&cfg) != 0) {
        notify_send("YouTube: Upload failed - config load error");
        return -1;
    }

    if (!cfg.auto_upload) {
        return 0;
    }

    notify_send("YouTube: Uploading clip...");

    char title[128];
    if (clip->title_id[0] != '\0') {
        snprintf(title, sizeof(title), "PS5 Capture - %s", clip->title_id);
    } else {
        snprintf(title, sizeof(title), "PS5 Gameplay Capture");
    }

    upload_result_t result;
    memset(&result, 0, sizeof(result));

    const char* privacy = cfg.default_privacy[0] ? cfg.default_privacy : "UNLISTED";
    int rc = youtube_upload_clip(&cfg, clip->filepath, title,
                                 "Uploaded from PlayStation 5 via PS5 YouTube Uploader",
                                 privacy, NULL, NULL, &result);

    if (rc == 0 && result.http_status == 200) {
        char msg[256];
        const char* url = result.youtube_url[0] ? result.youtube_url : result.video_id;
        snprintf(msg, sizeof(msg), "YouTube: Upload complete! (%s)", url);
        notify_send(msg);
        history_mark_uploaded(clip->filepath, result.video_id);
        return 0;
    } else {
        char msg[256];
        const char* err = result.error_message[0] ? result.error_message : "upload error";
        snprintf(msg, sizeof(msg), "YouTube: Upload failed - %s", err);
        notify_send(msg);
        return -1;
    }
}

int watcher_scan_once(void) {
    char root[512];
    watcher_clip_ready_cb cb = NULL;
    void* userdata = NULL;

    pthread_mutex_lock(&s_state_mutex);
    if (s_captures_root[0] == '\0') {
        pthread_mutex_unlock(&s_state_mutex);
        return -1;
    }
    snprintf(root, sizeof(root), "%s", s_captures_root);
    cb = s_cb;
    userdata = s_userdata;
    pthread_mutex_unlock(&s_state_mutex);

    capture_clip_t* found_clips = NULL;
    size_t found_count = 0;
    if (scan_existing_clips(root, &found_clips, &found_count) != 0) {
        return 0;
    }

    capture_clip_t* ready_clips = NULL;
    size_t ready_count = 0;
    size_t ready_cap = 0;

    pthread_mutex_lock(&s_state_mutex);

    /* Mark all currently tracked files as unseen */
    for (watched_file_node_t* n = s_tracked_files; n != NULL; n = n->next) {
        n->seen_in_scan = 0;
    }

    for (size_t i = 0; i < found_count; i++) {
        const capture_clip_t* clip = &found_clips[i];

        /* Deduplication: skip if already uploaded */
        if (history_is_uploaded(clip->filepath)) {
            continue;
        }

        /* Find tracking node */
        watched_file_node_t* node = NULL;
        for (watched_file_node_t* n = s_tracked_files; n != NULL; n = n->next) {
            if (strcmp(n->filepath, clip->filepath) == 0) {
                node = n;
                break;
            }
        }

        if (!node) {
            /* Newly discovered file */
            node = calloc(1, sizeof(watched_file_node_t));
            if (!node) {
                continue;
            }
            snprintf(node->filepath, sizeof(node->filepath), "%s", clip->filepath);
            snprintf(node->title_id, sizeof(node->title_id), "%s", clip->title_id);
            node->last_size = clip->size_bytes;
            node->last_mtime = clip->timestamp;
            node->stable_count = 0;
            node->dispatched = 0;
            node->seen_in_scan = 1;
            node->next = s_tracked_files;
            s_tracked_files = node;
            /* Do not trigger on initial detection */
            continue;
        }

        node->seen_in_scan = 1;

        if (node->dispatched) {
            continue;
        }

        if (clip->size_bytes == 0) {
            /* Zero-byte file: not ready */
            node->last_size = 0;
            node->stable_count = 0;
            continue;
        }

        if (clip->size_bytes != node->last_size) {
            /* File is still actively being written to */
            node->last_size = clip->size_bytes;
            node->last_mtime = clip->timestamp;
            node->stable_count = 0;
            continue;
        }

        /* Size is identical across consecutive checks and size > 0 */
        node->stable_count++;
        if (node->stable_count >= 1) {
            if (ready_count >= ready_cap) {
                size_t new_cap = (ready_cap == 0) ? 8 : ready_cap * 2;
                capture_clip_t* new_ready = realloc(ready_clips, new_cap * sizeof(capture_clip_t));
                if (!new_ready) {
                    continue;
                }
                ready_clips = new_ready;
                ready_cap = new_cap;
            }
            ready_clips[ready_count++] = *clip;
            node->dispatched = 1;
        }
    }

    /* Prune tracking entries for files removed from disk */
    watched_file_node_t** curr_ptr = &s_tracked_files;
    while (*curr_ptr) {
        watched_file_node_t* entry = *curr_ptr;
        if (!entry->seen_in_scan) {
            *curr_ptr = entry->next;
            free(entry);
        } else {
            curr_ptr = &entry->next;
        }
    }

    pthread_mutex_unlock(&s_state_mutex);

    free(found_clips);

    /* Dispatch ready callbacks without holding state mutex */
    for (size_t i = 0; i < ready_count; i++) {
        if (cb) {
            cb(&ready_clips[i], userdata);
        } else {
            watcher_process_clip_upload(&ready_clips[i]);
        }
    }

    free(ready_clips);
    return (int)ready_count;
}

static void* watcher_thread_main(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&s_state_mutex);
        if (!s_thread_running) {
            pthread_mutex_unlock(&s_state_mutex);
            break;
        }
        int interval = s_poll_interval_ms;
        pthread_mutex_unlock(&s_state_mutex);

        watcher_scan_once();

        pthread_mutex_lock(&s_state_mutex);
        if (!s_thread_running) {
            pthread_mutex_unlock(&s_state_mutex);
            break;
        }

        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct timespec ts;
        ts.tv_sec = tv.tv_sec + (interval / 1000);
        long nsec = (tv.tv_usec * 1000L) + (long)(interval % 1000) * 1000000L;
        ts.tv_sec += nsec / 1000000000L;
        ts.tv_nsec = nsec % 1000000000L;

        pthread_cond_timedwait(&s_cond, &s_state_mutex, &ts);
        if (!s_thread_running) {
            pthread_mutex_unlock(&s_state_mutex);
            break;
        }
        pthread_mutex_unlock(&s_state_mutex);
    }
    return NULL;
}

int watcher_start(void) {
    pthread_mutex_lock(&s_state_mutex);
    if (s_thread_running) {
        pthread_mutex_unlock(&s_state_mutex);
        return 0;
    }
    if (s_captures_root[0] == '\0') {
        pthread_mutex_unlock(&s_state_mutex);
        return -1;
    }

    s_thread_running = 1;
    int rc = pthread_create(&s_thread, NULL, watcher_thread_main, NULL);
    if (rc != 0) {
        s_thread_running = 0;
        pthread_mutex_unlock(&s_state_mutex);
        return -1;
    }
    s_thread_spawned = 1;
    pthread_mutex_unlock(&s_state_mutex);
    return 0;
}

void watcher_stop(void) {
    pthread_mutex_lock(&s_state_mutex);
    if (!s_thread_running && !s_thread_spawned) {
        watched_file_node_t* curr = s_tracked_files;
        while (curr) {
            watched_file_node_t* next = curr->next;
            free(curr);
            curr = next;
        }
        s_tracked_files = NULL;
        pthread_mutex_unlock(&s_state_mutex);
        return;
    }

    s_thread_running = 0;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_state_mutex);

    if (s_thread_spawned) {
        if (!pthread_equal(pthread_self(), s_thread)) {
            pthread_join(s_thread, NULL);
        } else {
            pthread_detach(s_thread);
        }
        s_thread_spawned = 0;
    }

    pthread_mutex_lock(&s_state_mutex);
    watched_file_node_t* curr = s_tracked_files;
    while (curr) {
        watched_file_node_t* next = curr->next;
        free(curr);
        curr = next;
    }
    s_tracked_files = NULL;
    pthread_mutex_unlock(&s_state_mutex);
}
