#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "api.h"
#include "server.h"
#include "config.h"
#include "watcher.h"
#include "youtube.h"
#include "notify.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

static char s_captures_root[512] = {0};
static pthread_mutex_t s_api_mutex = PTHREAD_MUTEX_INITIALIZER;

static int s_active_uploads = 0;
static pthread_mutex_t s_upload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_upload_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    char filepath[512];
    char title[128];
    char description[512];
    char privacy[32];
} upload_task_t;

static void handle_api_status(const char* req_body, char* resp_out, size_t max_resp) {
    (void)req_body;
    app_config_t cfg;
    int rc = config_load(&cfg);

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        snprintf(resp_out, max_resp, "{\"error\":\"out of memory\"}");
        return;
    }

    int authenticated = 0;
    if (rc == 0 && cfg.cookies[0] != '\0' && cfg.session_token[0] != '\0') {
        authenticated = 1;
    }

    cJSON_AddBoolToObject(root, "authenticated", authenticated);
    cJSON_AddStringToObject(root, "channel_name", (rc == 0) ? cfg.channel_name : "");
    cJSON_AddStringToObject(root, "channel_title", (rc == 0) ? cfg.channel_name : "");
    cJSON_AddStringToObject(root, "channel_id", (rc == 0) ? cfg.channel_id : "");
    cJSON_AddBoolToObject(root, "auto_upload", (rc == 0) ? cfg.auto_upload : 0);
    cJSON_AddStringToObject(root, "default_privacy",
                           (rc == 0 && cfg.default_privacy[0]) ? cfg.default_privacy : "UNLISTED");

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        snprintf(resp_out, max_resp, "%s", json_str);
        free(json_str);
    } else {
        snprintf(resp_out, max_resp, "{\"error\":\"json serialization error\"}");
    }
}

static void handle_api_clips(const char* req_body, char* resp_out, size_t max_resp) {
    (void)req_body;

    char root_path[512];
    pthread_mutex_lock(&s_api_mutex);
    snprintf(root_path, sizeof(root_path), "%s", s_captures_root);
    pthread_mutex_unlock(&s_api_mutex);

    capture_clip_t* clips = NULL;
    size_t count = 0;
    int rc = scan_existing_clips(root_path, &clips, &count);

    cJSON* root = cJSON_CreateArray();
    if (!root) {
        if (clips) free(clips);
        snprintf(resp_out, max_resp, "[]");
        return;
    }

    if (rc == 0 && clips && count > 0) {
        for (size_t i = 0; i < count; i++) {
            cJSON* item = cJSON_CreateObject();
            if (item) {
                cJSON_AddStringToObject(item, "filepath", clips[i].filepath);
                cJSON_AddStringToObject(item, "title_id", clips[i].title_id);
                cJSON_AddNumberToObject(item, "timestamp", (double)clips[i].timestamp);
                cJSON_AddNumberToObject(item, "size_bytes", (double)clips[i].size_bytes);
                cJSON_AddItemToArray(root, item);
            }
        }
    }

    if (clips) {
        free(clips);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        snprintf(resp_out, max_resp, "%s", json_str);
        free(json_str);
    } else {
        snprintf(resp_out, max_resp, "[]");
    }
}

static void handle_api_config(const char* req_body, char* resp_out, size_t max_resp) {
    if (!req_body || !*req_body) {
        snprintf(resp_out, max_resp, "{\"error\":\"Missing request body\",\"success\":false}");
        return;
    }

    cJSON* root = cJSON_Parse(req_body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Invalid JSON\",\"success\":false}");
        return;
    }

    app_config_t cfg;
    if (config_load(&cfg) != 0) {
        cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Failed to load configuration\",\"success\":false}");
        return;
    }

    cJSON* item = cJSON_GetObjectItem(root, "auto_upload");
    if (item && cJSON_IsBool(item)) {
        cfg.auto_upload = cJSON_IsTrue(item) ? 1 : 0;
    }

    item = cJSON_GetObjectItem(root, "default_privacy");
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg.default_privacy, sizeof(cfg.default_privacy), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "channel_name");
    if (!item) item = cJSON_GetObjectItem(root, "channel_title");
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg.channel_name, sizeof(cfg.channel_name), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "server_port");
    if (item && cJSON_IsNumber(item)) {
        cfg.server_port = item->valueint;
    }

    cJSON_Delete(root);

    if (config_save(&cfg) != 0) {
        snprintf(resp_out, max_resp, "{\"error\":\"Failed to save configuration\",\"success\":false}");
        return;
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", 1);
    cJSON_AddBoolToObject(resp, "auto_upload", cfg.auto_upload);
    cJSON_AddStringToObject(resp, "default_privacy", cfg.default_privacy);

    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (s) {
        snprintf(resp_out, max_resp, "%s", s);
        free(s);
    } else {
        snprintf(resp_out, max_resp, "{\"success\":true}");
    }
}

static void handle_api_auth(const char* req_body, char* resp_out, size_t max_resp) {
    if (!req_body || !*req_body) {
        snprintf(resp_out, max_resp, "{\"error\":\"Missing request body\",\"success\":false}");
        return;
    }

    cJSON* root = cJSON_Parse(req_body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Invalid JSON\",\"success\":false}");
        return;
    }

    cJSON* cookies_item = cJSON_GetObjectItem(root, "cookies");
    cJSON* token_item = cJSON_GetObjectItem(root, "session_token");

    if (!cookies_item || !cJSON_IsString(cookies_item) ||
        !token_item || !cJSON_IsString(token_item)) {
        cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Missing cookies or session_token\",\"success\":false}");
        return;
    }

    app_config_t cfg;
    if (config_load(&cfg) != 0) {
        cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Failed to load configuration\",\"success\":false}");
        return;
    }

    snprintf(cfg.cookies, sizeof(cfg.cookies), "%s", cookies_item->valuestring);
    snprintf(cfg.session_token, sizeof(cfg.session_token), "%s", token_item->valuestring);

    cJSON* chan_item = cJSON_GetObjectItem(root, "channel_name");
    if (!chan_item) chan_item = cJSON_GetObjectItem(root, "channel_title");
    if (chan_item && cJSON_IsString(chan_item) && chan_item->valuestring[0]) {
        snprintf(cfg.channel_name, sizeof(cfg.channel_name), "%s", chan_item->valuestring);
    } else if (cfg.channel_name[0] == '\0') {
        snprintf(cfg.channel_name, sizeof(cfg.channel_name), "PlayStation User");
    }

    cJSON* id_item = cJSON_GetObjectItem(root, "channel_id");
    if (id_item && cJSON_IsString(id_item) && id_item->valuestring[0]) {
        snprintf(cfg.channel_id, sizeof(cfg.channel_id), "%s", id_item->valuestring);
    }

    cJSON_Delete(root);

    if (config_save(&cfg) != 0) {
        snprintf(resp_out, max_resp, "{\"error\":\"Failed to save configuration\",\"success\":false}");
        return;
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", 1);
    cJSON_AddStringToObject(resp, "channel_name", cfg.channel_name);
    cJSON_AddStringToObject(resp, "channel_title", cfg.channel_name);
    if (cfg.channel_id[0]) {
        cJSON_AddStringToObject(resp, "channel_id", cfg.channel_id);
    }

    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (s) {
        snprintf(resp_out, max_resp, "%s", s);
        free(s);
    } else {
        snprintf(resp_out, max_resp, "{\"success\":true}");
    }
}

static void* upload_worker_thread(void* arg) {
    upload_task_t* task = (upload_task_t*)arg;
    if (!task) {
        pthread_mutex_lock(&s_upload_mutex);
        s_active_uploads--;
        if (s_active_uploads == 0) {
            pthread_cond_broadcast(&s_upload_cond);
        }
        pthread_mutex_unlock(&s_upload_mutex);
        return NULL;
    }

    app_config_t cfg;
    if (config_load(&cfg) != 0) {
        notify_send("YouTube: Upload failed - config load error");
        free(task);
        pthread_mutex_lock(&s_upload_mutex);
        s_active_uploads--;
        if (s_active_uploads == 0) {
            pthread_cond_broadcast(&s_upload_cond);
        }
        pthread_mutex_unlock(&s_upload_mutex);
        return NULL;
    }

    notify_send("YouTube: Uploading clip...");

    upload_result_t result;
    memset(&result, 0, sizeof(result));

    const char* privacy = task->privacy[0] ? task->privacy :
                          (cfg.default_privacy[0] ? cfg.default_privacy : "UNLISTED");
    const char* title = task->title[0] ? task->title : "PS5 Gameplay Capture";
    const char* desc = task->description[0] ? task->description :
                       "Uploaded from PlayStation 5 via PS5 YouTube Uploader";

    int rc = youtube_upload_clip(&cfg, task->filepath, title, desc, privacy, NULL, NULL, &result);
    if (rc == 0 && result.http_status == 200) {
        char msg[256];
        const char* url = result.youtube_url[0] ? result.youtube_url : result.video_id;
        snprintf(msg, sizeof(msg), "YouTube: Upload complete! (%s)", url);
        notify_send(msg);
        history_mark_uploaded(task->filepath, result.video_id);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "YouTube: Upload failed - %s",
                 result.error_message[0] ? result.error_message : "network/server error");
        notify_send(msg);
    }

    free(task);

    pthread_mutex_lock(&s_upload_mutex);
    s_active_uploads--;
    if (s_active_uploads == 0) {
        pthread_cond_broadcast(&s_upload_cond);
    }
    pthread_mutex_unlock(&s_upload_mutex);

    return NULL;
}

static void handle_api_upload(const char* req_body, char* resp_out, size_t max_resp) {
    if (!req_body || !*req_body) {
        snprintf(resp_out, max_resp, "{\"error\":\"Missing request body\",\"success\":false}");
        return;
    }

    cJSON* root = cJSON_Parse(req_body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Invalid JSON\",\"success\":false}");
        return;
    }

    cJSON* file_item = cJSON_GetObjectItem(root, "filepath");
    if (!file_item || !cJSON_IsString(file_item) || !file_item->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Missing filepath\",\"success\":false}");
        return;
    }

    upload_task_t* task = calloc(1, sizeof(upload_task_t));
    if (!task) {
        cJSON_Delete(root);
        snprintf(resp_out, max_resp, "{\"error\":\"Out of memory\",\"success\":false}");
        return;
    }

    snprintf(task->filepath, sizeof(task->filepath), "%s", file_item->valuestring);

    cJSON* item = cJSON_GetObjectItem(root, "title");
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(task->title, sizeof(task->title), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "description");
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(task->description, sizeof(task->description), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "privacy");
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(task->privacy, sizeof(task->privacy), "%s", item->valuestring);
    }

    cJSON_Delete(root);

    pthread_mutex_lock(&s_upload_mutex);
    s_active_uploads++;
    pthread_mutex_unlock(&s_upload_mutex);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int rc = pthread_create(&tid, &attr, upload_worker_thread, task);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free(task);
        pthread_mutex_lock(&s_upload_mutex);
        s_active_uploads--;
        if (s_active_uploads == 0) {
            pthread_cond_broadcast(&s_upload_cond);
        }
        pthread_mutex_unlock(&s_upload_mutex);

        snprintf(resp_out, max_resp, "{\"error\":\"Failed to spawn upload thread\",\"success\":false}");
        return;
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "queued");
    cJSON_AddBoolToObject(resp, "success", 1);
    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (s) {
        snprintf(resp_out, max_resp, "%s", s);
        free(s);
    } else {
        snprintf(resp_out, max_resp, "{\"status\":\"queued\",\"success\":true}");
    }
}

int api_init(const char* captures_root) {
    if (!captures_root || !*captures_root) {
        return -1;
    }

    pthread_mutex_lock(&s_api_mutex);
    snprintf(s_captures_root, sizeof(s_captures_root), "%s", captures_root);
    pthread_mutex_unlock(&s_api_mutex);

    if (server_register_route("GET", "/api/status", handle_api_status) != 0) return -1;
    if (server_register_route("GET", "/api/clips", handle_api_clips) != 0) return -1;
    if (server_register_route("POST", "/api/config", handle_api_config) != 0) return -1;
    if (server_register_route("POST", "/api/auth", handle_api_auth) != 0) return -1;
    if (server_register_route("POST", "/api/upload", handle_api_upload) != 0) return -1;

    return 0;
}

void api_cleanup(void) {
    // Wait up to 1 second for any pending upload worker threads to complete
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec ts;
    ts.tv_sec = now.tv_sec + 1;
    ts.tv_nsec = now.tv_usec * 1000L;

    pthread_mutex_lock(&s_upload_mutex);
    while (s_active_uploads > 0) {
        int wait_rc = pthread_cond_timedwait(&s_upload_cond, &s_upload_mutex, &ts);
        if (wait_rc != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&s_upload_mutex);

    pthread_mutex_lock(&s_api_mutex);
    s_captures_root[0] = '\0';
    pthread_mutex_unlock(&s_api_mutex);
}
