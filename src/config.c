#include "config.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#define DEFAULT_STORAGE_DIR "/data/youtube_uploader"

static char s_base_dir[1024] = DEFAULT_STORAGE_DIR;
static cJSON* s_history_json = NULL;
static pthread_mutex_t s_config_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Recursively creates directories with mode 0777.
 */
static int mkdir_p(const char* path, mode_t mode) {
    char tmp[1024];
    char* p = NULL;
    size_t len;

    if (!path || !*path) {
        return -1;
    }

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0) {
                if (errno != EEXIST) {
                    return -1;
                }
                struct stat st;
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    return -1;
                }
            } else {
                chmod(tmp, mode);
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0) {
        if (errno != EEXIST) {
            return -1;
        }
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return -1;
        }
    } else {
        chmod(tmp, mode);
    }

    return 0;
}

/**
 * Reads an entire file into a null-terminated string buffer.
 * Caller must free() the returned buffer.
 */
static char* read_file_to_string(const char* filepath) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char* buf = (char*)malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, size, fp);
    fclose(fp);
    buf[read_bytes] = '\0';
    return buf;
}

/**
 * Atomically writes content to dest_path via temp_path.
 */
static int write_atomic(const char* temp_path, const char* dest_path, const char* content) {
    FILE* fp = fopen(temp_path, "wb");
    if (!fp) {
        return -1;
    }

    size_t len = strlen(content);
    if (fwrite(content, 1, len, fp) != len) {
        fclose(fp);
        unlink(temp_path);
        return -1;
    }

    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(fp);

    if (rename(temp_path, dest_path) != 0) {
        unlink(temp_path);
        return -1;
    }

    chmod(dest_path, 0666);
    return 0;
}

/**
 * Loads or initializes the upload history JSON structure.
 * Must be called with s_config_mutex locked.
 */
static void load_history_locked(void) {
    if (s_history_json) {
        cJSON_Delete(s_history_json);
        s_history_json = NULL;
    }

    char history_path[1024];
    snprintf(history_path, sizeof(history_path), "%s/uploaded.json", s_base_dir);

    char* content = read_file_to_string(history_path);
    if (content) {
        cJSON* parsed = cJSON_Parse(content);
        free(content);
        if (parsed && cJSON_IsObject(parsed)) {
            s_history_json = parsed;
            return;
        }
        if (parsed) {
            cJSON_Delete(parsed);
        }
    }

    s_history_json = cJSON_CreateObject();
}

int config_init(const char* base_dir) {
    pthread_mutex_lock(&s_config_mutex);

    if (base_dir && *base_dir) {
        snprintf(s_base_dir, sizeof(s_base_dir), "%s", base_dir);
    } else {
        snprintf(s_base_dir, sizeof(s_base_dir), "%s", DEFAULT_STORAGE_DIR);
    }

    // Strip trailing slashes
    size_t len = strlen(s_base_dir);
    while (len > 1 && s_base_dir[len - 1] == '/') {
        s_base_dir[len - 1] = '\0';
        len--;
    }

    // Review Focus #5: Ensure directory created recursively with mode 0777
    if (mkdir_p(s_base_dir, 0777) != 0) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    // Review Focus #4: Load persistent upload history
    load_history_locked();

    pthread_mutex_unlock(&s_config_mutex);
    return 0;
}

int config_load(app_config_t* cfg) {
    if (!cfg) {
        return -1;
    }

    // Set default values
    memset(cfg, 0, sizeof(*cfg));
    cfg->server_port = 8888;
    snprintf(cfg->default_privacy, sizeof(cfg->default_privacy), "UNLISTED");
    cfg->auto_upload = 1;

    pthread_mutex_lock(&s_config_mutex);

    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/config.json", s_base_dir);

    if (access(config_path, F_OK) != 0) {
        // File does not exist yet; return defaults with success
        pthread_mutex_unlock(&s_config_mutex);
        return 0;
    }

    char* content = read_file_to_string(config_path);
    if (!content) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    cJSON* item = NULL;

    item = cJSON_GetObjectItem(root, "cookies");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg->cookies, sizeof(cfg->cookies), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "session_token");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg->session_token, sizeof(cfg->session_token), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "innertube_key");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg->innertube_key, sizeof(cfg->innertube_key), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "channel_id");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg->channel_id, sizeof(cfg->channel_id), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "channel_name");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg->channel_name, sizeof(cfg->channel_name), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "auto_upload");
    if (cJSON_IsNumber(item)) {
        cfg->auto_upload = item->valueint;
    } else if (cJSON_IsBool(item)) {
        cfg->auto_upload = cJSON_IsTrue(item) ? 1 : 0;
    }

    item = cJSON_GetObjectItem(root, "default_privacy");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(cfg->default_privacy, sizeof(cfg->default_privacy), "%s", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "server_port");
    if (cJSON_IsNumber(item)) {
        cfg->server_port = item->valueint;
    }

    cJSON_Delete(root);
    pthread_mutex_unlock(&s_config_mutex);
    return 0;
}

int config_save(const app_config_t* cfg) {
    if (!cfg) {
        return -1;
    }

    pthread_mutex_lock(&s_config_mutex);

    // Ensure directory exists
    if (mkdir_p(s_base_dir, 0777) != 0) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    cJSON_AddStringToObject(root, "cookies", cfg->cookies);
    cJSON_AddStringToObject(root, "session_token", cfg->session_token);
    cJSON_AddStringToObject(root, "innertube_key", cfg->innertube_key);
    cJSON_AddStringToObject(root, "channel_id", cfg->channel_id);
    cJSON_AddStringToObject(root, "channel_name", cfg->channel_name);
    cJSON_AddNumberToObject(root, "auto_upload", cfg->auto_upload);
    cJSON_AddStringToObject(root, "default_privacy", cfg->default_privacy);
    cJSON_AddNumberToObject(root, "server_port", cfg->server_port);

    char* rendered = cJSON_Print(root);
    cJSON_Delete(root);

    if (!rendered) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    char config_path[1024];
    char tmp_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/config.json", s_base_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/config.json.tmp", s_base_dir);

    int ret = write_atomic(tmp_path, config_path, rendered);
    cJSON_free(rendered);

    pthread_mutex_unlock(&s_config_mutex);
    return ret;
}

int history_is_uploaded(const char* filepath) {
    if (!filepath || !*filepath) {
        return 0;
    }

    pthread_mutex_lock(&s_config_mutex);

    if (!s_history_json) {
        load_history_locked();
    }

    int found = 0;
    if (s_history_json) {
        cJSON* item = cJSON_GetObjectItem(s_history_json, filepath);
        if (item != NULL) {
            found = 1;
        }
    }

    pthread_mutex_unlock(&s_config_mutex);
    return found;
}

int history_mark_uploaded(const char* filepath, const char* video_id) {
    if (!filepath || !*filepath) {
        return -1;
    }

    pthread_mutex_lock(&s_config_mutex);

    if (!s_history_json) {
        load_history_locked();
    }

    // Ensure directory exists
    if (mkdir_p(s_base_dir, 0777) != 0) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    // If item already exists, delete it first to replace
    cJSON_DeleteItemFromObject(s_history_json, filepath);

    cJSON* entry = cJSON_CreateObject();
    if (!entry) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    cJSON_AddStringToObject(entry, "video_id", video_id ? video_id : "");
    cJSON_AddNumberToObject(entry, "timestamp", (double)time(NULL));

    cJSON_AddItemToObject(s_history_json, filepath, entry);

    char* rendered = cJSON_Print(s_history_json);
    if (!rendered) {
        pthread_mutex_unlock(&s_config_mutex);
        return -1;
    }

    char history_path[1024];
    char tmp_path[1024];
    snprintf(history_path, sizeof(history_path), "%s/uploaded.json", s_base_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/uploaded.json.tmp", s_base_dir);

    // Review Focus #4: Persistent deduplication in uploaded.json
    int ret = write_atomic(tmp_path, history_path, rendered);
    cJSON_free(rendered);

    pthread_mutex_unlock(&s_config_mutex);
    return ret;
}
