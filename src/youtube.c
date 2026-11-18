#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "youtube.h"
#include "http_client.h"
#include "crypto.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_SCOTTY_ORIGIN "https://studio.youtube.com"
#define DEFAULT_SCOTTY_INIT_URL "https://upload.youtube.com/upload/studio?authuser=0"
#define DEFAULT_INNERTUBE_URL "https://studio.youtube.com/youtubei/v1/upload/createvideo"
#define DEFAULT_CHUNK_SIZE (8 * 1024 * 1024) // 8 MB

static char g_scotty_init_url[512] = {0};
static char g_innertube_url[512] = {0};
static size_t g_chunk_size = DEFAULT_CHUNK_SIZE;

static int g_last_http_status = 0;
static char g_last_error_message[256] = {0};

void youtube_set_base_urls(const char* scotty_init_url, const char* innertube_createvideo_url) {
    if (scotty_init_url && *scotty_init_url) {
        strncpy(g_scotty_init_url, scotty_init_url, sizeof(g_scotty_init_url) - 1);
        g_scotty_init_url[sizeof(g_scotty_init_url) - 1] = '\0';
    } else {
        g_scotty_init_url[0] = '\0';
    }

    if (innertube_createvideo_url && *innertube_createvideo_url) {
        strncpy(g_innertube_url, innertube_createvideo_url, sizeof(g_innertube_url) - 1);
        g_innertube_url[sizeof(g_innertube_url) - 1] = '\0';
    } else {
        g_innertube_url[0] = '\0';
    }
}

size_t youtube_get_chunk_size(void) {
    return g_chunk_size;
}

void youtube_set_chunk_size(size_t chunk_size) {
    if (chunk_size > 0) {
        g_chunk_size = chunk_size;
    }
}

static const char* get_scotty_init_url(void) {
    if (g_scotty_init_url[0] != '\0') {
        return g_scotty_init_url;
    }
    const char* env = getenv("PS5_YOUTUBE_SCOTTY_URL");
    if (env && *env) {
        return env;
    }
    return DEFAULT_SCOTTY_INIT_URL;
}

static const char* get_innertube_url(void) {
    if (g_innertube_url[0] != '\0') {
        return g_innertube_url;
    }
    const char* env = getenv("PS5_YOUTUBE_INNERTUBE_URL");
    if (env && *env) {
        return env;
    }
    return DEFAULT_INNERTUBE_URL;
}

static int get_auth_header(const app_config_t* cfg, char* auth_out, size_t max_len) {
    if (!cfg || !auth_out || max_len < 64) return -1;

    char sapisid[256];
    if (extract_cookie_value(cfg->cookies, "SAPISID", sapisid, sizeof(sapisid)) != 0) {
        if (extract_cookie_value(cfg->cookies, "__Secure-3PAPISID", sapisid, sizeof(sapisid)) != 0) {
            return -1; // SAPISID missing
        }
    }

    char hash_buf[128];
    long now = (long)time(NULL);
    if (generate_sapisid_hash(sapisid, DEFAULT_SCOTTY_ORIGIN, now, hash_buf, sizeof(hash_buf)) != 0) {
        return -1;
    }

    snprintf(auth_out, max_len, "SAPISIDHASH %s", hash_buf);
    return 0;
}

int youtube_init_upload_session(const app_config_t* cfg, const char* frontend_id, char* upload_url_out, size_t max_len) {
    g_last_http_status = 0;
    g_last_error_message[0] = '\0';

    if (!cfg || !upload_url_out || max_len == 0) return -1;

    char auth_header[256];
    if (get_auth_header(cfg, auth_header, sizeof(auth_header)) != 0) {
        g_last_http_status = 401;
        snprintf(g_last_error_message, sizeof(g_last_error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        return -1;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddStringToObject(root, "frontendUploadId", frontend_id ? frontend_id : "ps5_upload");
    char* body_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body_json) return -1;

    http_header_t headers[7];
    size_t h_idx = 0;

    strncpy(headers[h_idx].name, "Authorization", sizeof(headers[h_idx].name));
    headers[h_idx].value = auth_header;
    h_idx++;

    strncpy(headers[h_idx].name, "x-goog-upload-command", sizeof(headers[h_idx].name));
    headers[h_idx].value = "start";
    h_idx++;

    strncpy(headers[h_idx].name, "x-goog-upload-protocol", sizeof(headers[h_idx].name));
    headers[h_idx].value = "resumable";
    h_idx++;

    strncpy(headers[h_idx].name, "Origin", sizeof(headers[h_idx].name));
    headers[h_idx].value = DEFAULT_SCOTTY_ORIGIN;
    h_idx++;

    strncpy(headers[h_idx].name, "X-Goog-AuthUser", sizeof(headers[h_idx].name));
    headers[h_idx].value = "0";
    h_idx++;

    strncpy(headers[h_idx].name, "Cookie", sizeof(headers[h_idx].name));
    headers[h_idx].value = (char*)cfg->cookies;
    h_idx++;

    strncpy(headers[h_idx].name, "Content-Type", sizeof(headers[h_idx].name));
    headers[h_idx].value = "application/json; charset=UTF-8";
    h_idx++;

    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = get_scotty_init_url();
    req.headers = headers;
    req.header_count = h_idx;
    req.body = body_json;
    req.body_len = strlen(body_json);
    req.timeout_sec = 10;

    http_response_t resp;
    http_response_init(&resp);

    int rc = http_client_request(&req, &resp);
    free(body_json);

    if (rc != 0) {
        http_response_cleanup(&resp);
        g_last_http_status = 500;
        snprintf(g_last_error_message, sizeof(g_last_error_message), "Network connection failed during session init");
        return -1;
    }

    g_last_http_status = resp.status_code;

    if (resp.status_code == 401) {
        snprintf(g_last_error_message, sizeof(g_last_error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        http_response_cleanup(&resp);
        return -1;
    }

    if (resp.status_code != 200 && resp.status_code != 201) {
        snprintf(g_last_error_message, sizeof(g_last_error_message), "Scotty session init failed with HTTP %d", resp.status_code);
        http_response_cleanup(&resp);
        return -1;
    }

    const char* upload_url = http_response_get_header(&resp, "x-goog-upload-url");
    if (!upload_url) {
        upload_url = http_response_get_header(&resp, "Location");
    }

    if (!upload_url || strlen(upload_url) >= max_len) {
        http_response_cleanup(&resp);
        snprintf(g_last_error_message, sizeof(g_last_error_message), "Missing or oversized x-goog-upload-url in Scotty response");
        return -1;
    }

    strncpy(upload_url_out, upload_url, max_len - 1);
    upload_url_out[max_len - 1] = '\0';

    http_response_cleanup(&resp);
    return 0;
}

static int parse_scotty_resource_id(const char* body, char* scotty_id_out, size_t max_len) {
    if (!body || !scotty_id_out || max_len == 0) return -1;

    cJSON* json = cJSON_Parse(body);
    if (!json) return -1;

    const char* res_id = NULL;

    cJSON* top_id = cJSON_GetObjectItem(json, "scottyResourceId");
    if (top_id && cJSON_IsString(top_id) && top_id->valuestring) {
        res_id = top_id->valuestring;
    } else {
        cJSON* res_obj = cJSON_GetObjectItem(json, "resourceId");
        if (res_obj) {
            cJSON* scotty_obj = cJSON_GetObjectItem(res_obj, "scottyResourceId");
            if (scotty_obj) {
                cJSON* id_item = cJSON_GetObjectItem(scotty_obj, "id");
                if (id_item && cJSON_IsString(id_item) && id_item->valuestring) {
                    res_id = id_item->valuestring;
                }
            }
        }
    }

    int rc = -1;
    if (res_id && strlen(res_id) < max_len) {
        strncpy(scotty_id_out, res_id, max_len - 1);
        scotty_id_out[max_len - 1] = '\0';
        rc = 0;
    }

    cJSON_Delete(json);
    return rc;
}

int youtube_stream_file(const app_config_t* cfg, const char* upload_url, const char* filepath, char* scotty_id_out, size_t max_len, upload_progress_cb cb, void* userdata) {
    g_last_http_status = 0;
    g_last_error_message[0] = '\0';

    if (!cfg || !upload_url || !filepath || !scotty_id_out || max_len == 0) return -1;

    struct stat st;
    if (stat(filepath, &st) != 0 || st.st_size <= 0) {
        snprintf(g_last_error_message, sizeof(g_last_error_message), "Capture file inaccessible or empty: %s", filepath);
        return -1;
    }
    size_t total_size = (size_t)st.st_size;

    char auth_header[256];
    if (get_auth_header(cfg, auth_header, sizeof(auth_header)) != 0) {
        g_last_http_status = 401;
        snprintf(g_last_error_message, sizeof(g_last_error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        return -1;
    }

    size_t current_offset = 0;
    int retry_count = 0;
    const int max_retries = 5;

    while (current_offset <= total_size) {
        size_t remaining = (total_size >= current_offset) ? (total_size - current_offset) : 0;
        size_t chunk_len = (remaining > g_chunk_size) ? g_chunk_size : remaining;
        int is_final = (current_offset + chunk_len >= total_size);

        char offset_str[32];
        snprintf(offset_str, sizeof(offset_str), "%zu", current_offset);

        char cl_str[32];
        snprintf(cl_str, sizeof(cl_str), "%zu", chunk_len);

        const char* cmd = is_final ? "upload, finalize" : "upload";

        http_header_t headers[5];
        size_t h_idx = 0;

        strncpy(headers[h_idx].name, "Authorization", sizeof(headers[h_idx].name));
        headers[h_idx].value = auth_header;
        h_idx++;

        strncpy(headers[h_idx].name, "x-goog-upload-command", sizeof(headers[h_idx].name));
        headers[h_idx].value = (char*)cmd;
        h_idx++;

        strncpy(headers[h_idx].name, "x-goog-upload-offset", sizeof(headers[h_idx].name));
        headers[h_idx].value = offset_str;
        h_idx++;

        strncpy(headers[h_idx].name, "Content-Type", sizeof(headers[h_idx].name));
        headers[h_idx].value = "application/octet-stream";
        h_idx++;

        strncpy(headers[h_idx].name, "Content-Length", sizeof(headers[h_idx].name));
        headers[h_idx].value = cl_str;
        h_idx++;

        http_request_t req;
        memset(&req, 0, sizeof(req));
        req.method = "POST";
        req.url = upload_url;
        req.headers = headers;
        req.header_count = h_idx;
        if (chunk_len > 0) {
            req.body_filepath = filepath;
            req.file_offset = current_offset;
            req.file_length = chunk_len;
        } else {
            req.body = "";
            req.body_len = 0;
        }
        req.progress_cb = (http_progress_cb)cb;
        req.userdata = userdata;
        req.timeout_sec = 15;

        http_response_t resp;
        http_response_init(&resp);

        int req_rc = http_client_request(&req, &resp);

        if (req_rc == 0) {
            g_last_http_status = resp.status_code;

            if (resp.status_code == 401) {
                snprintf(g_last_error_message, sizeof(g_last_error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
                http_response_cleanup(&resp);
                return -1; // Stop immediately, no retry
            }

            if (resp.status_code == 200 || resp.status_code == 201) {
                retry_count = 0;
                if (is_final) {
                    // Extract scottyResourceId
                    if (parse_scotty_resource_id(resp.body, scotty_id_out, max_len) != 0) {
                        snprintf(g_last_error_message, sizeof(g_last_error_message), "Failed to parse scottyResourceId from response");
                        http_response_cleanup(&resp);
                        return -1;
                    }
                    http_response_cleanup(&resp);
                    return 0; // Stream complete!
                } else {
                    current_offset += chunk_len;
                    http_response_cleanup(&resp);
                    continue;
                }
            }
        }

        // Network error or unexpected status code: Mid-stream resume flow (Review Focus #3)
        http_response_cleanup(&resp);
        retry_count++;
        if (retry_count > max_retries) {
            snprintf(g_last_error_message, sizeof(g_last_error_message), "Max retries exceeded during Scotty stream");
            return -1;
        }

        // Wait briefly before querying offset
        usleep(100000); // 100ms

        // Issue resume query: x-goog-upload-command: query
        http_header_t q_headers[3];
        size_t q_idx = 0;

        strncpy(q_headers[q_idx].name, "Authorization", sizeof(q_headers[q_idx].name));
        q_headers[q_idx].value = auth_header;
        q_idx++;

        strncpy(q_headers[q_idx].name, "x-goog-upload-command", sizeof(q_headers[q_idx].name));
        q_headers[q_idx].value = "query";
        q_idx++;

        strncpy(q_headers[q_idx].name, "Content-Length", sizeof(q_headers[q_idx].name));
        q_headers[q_idx].value = "0";
        q_idx++;

        http_request_t q_req;
        memset(&q_req, 0, sizeof(q_req));
        q_req.method = "POST";
        q_req.url = upload_url;
        q_req.headers = q_headers;
        q_req.header_count = q_idx;
        q_req.body = "";
        q_req.body_len = 0;
        q_req.timeout_sec = 10;

        http_response_t q_resp;
        http_response_init(&q_resp);

        int q_rc = http_client_request(&q_req, &q_resp);

        // Review Focus #2 / Issue 5: If query returns 401, halt immediately!
        if (q_rc == 0 && q_resp.status_code == 401) {
            g_last_http_status = 401;
            snprintf(g_last_error_message, sizeof(g_last_error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
            http_response_cleanup(&q_resp);
            return -1;
        }

        if (q_rc == 0 && (q_resp.status_code == 200 || q_resp.status_code == 201)) {
            const char* status_header = http_response_get_header(&q_resp, "x-goog-upload-status");
            if (status_header && strcasecmp(status_header, "final") == 0) {
                // Server already finalized - verify return code of parse_scotty_resource_id (Issue 2)
                if (parse_scotty_resource_id(q_resp.body, scotty_id_out, max_len) == 0) {
                    http_response_cleanup(&q_resp);
                    return 0;
                }
            }

            const char* size_recvd_str = http_response_get_header(&q_resp, "x-goog-upload-size-received");
            if (size_recvd_str) {
                size_t recvd = (size_t)strtoull(size_recvd_str, NULL, 10);
                if (recvd <= total_size) {
                    current_offset = recvd; // Resume from byte offset reported by Scotty
                    // Note: if recvd == total_size, next loop iteration will send a 0-length "upload, finalize" chunk at offset total_size (Issue 2)
                }
            }
        }
        http_response_cleanup(&q_resp);
    }

    return -1;
}

int youtube_finalize_video(const app_config_t* cfg, const char* frontend_id, const char* scotty_id, const char* title, const char* description, const char* privacy, upload_result_t* result) {
    g_last_http_status = 0;
    g_last_error_message[0] = '\0';

    if (!cfg || !result) return -1;
    memset(result, 0, sizeof(*result));

    // Review Focus #2 / Issue 3: check empty credentials only (no test mock sentinel string checks)
    if (!cfg->session_token[0]) {
        result->http_status = 401;
        snprintf(result->error_message, sizeof(result->error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        return -1;
    }

    char auth_header[256];
    if (get_auth_header(cfg, auth_header, sizeof(auth_header)) != 0) {
        result->http_status = 401;
        snprintf(result->error_message, sizeof(result->error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        return -1;
    }

    if (!scotty_id || !*scotty_id) {
        result->http_status = 400;
        snprintf(result->error_message, sizeof(result->error_message), "Missing scotty_id for video finalization");
        return -1;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON* context = cJSON_CreateObject();
    cJSON* client = cJSON_CreateObject();
    cJSON_AddStringToObject(client, "clientName", "WEB_REMIX");
    cJSON_AddStringToObject(client, "clientVersion", "1.20231118.01.00");
    cJSON_AddStringToObject(client, "hl", "en");
    cJSON_AddStringToObject(client, "gl", "US");
    cJSON_AddItemToObject(context, "client", client);

    cJSON* req_obj = cJSON_CreateObject();
    cJSON* session_info = cJSON_CreateObject();
    cJSON_AddStringToObject(session_info, "token", cfg->session_token);
    cJSON_AddItemToObject(req_obj, "sessionInfo", session_info);
    cJSON_AddItemToObject(context, "request", req_obj);
    cJSON_AddItemToObject(root, "context", context);

    cJSON* res_id_obj = cJSON_CreateObject();
    cJSON* scotty_res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(scotty_res_obj, "id", scotty_id);
    cJSON_AddItemToObject(res_id_obj, "scottyResourceId", scotty_res_obj);
    cJSON_AddItemToObject(root, "resourceId", res_id_obj);

    if (frontend_id && *frontend_id) {
        cJSON_AddStringToObject(root, "frontendUploadId", frontend_id);
    }

    cJSON* initial_metadata = cJSON_CreateObject();
    cJSON* title_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(title_obj, "newTitle", (title && *title) ? title : "PS5 Gameplay Capture");
    cJSON_AddItemToObject(initial_metadata, "title", title_obj);

    cJSON* desc_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(desc_obj, "newDescription", description ? description : "");
    cJSON_AddItemToObject(initial_metadata, "description", desc_obj);

    cJSON* priv_obj = cJSON_CreateObject();
    const char* priv = (privacy && *privacy) ? privacy : (cfg->default_privacy[0] ? cfg->default_privacy : "UNLISTED");
    cJSON_AddStringToObject(priv_obj, "newPrivacy", priv);
    cJSON_AddItemToObject(initial_metadata, "privacy", priv_obj);

    cJSON_AddItemToObject(root, "initialMetadata", initial_metadata);

    char* body_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body_json) return -1;

    char full_url[1024];
    const char* base_url = get_innertube_url();
    if (strchr(base_url, '?')) {
        snprintf(full_url, sizeof(full_url), "%s&key=%s&alt=json", base_url, cfg->innertube_key);
    } else {
        snprintf(full_url, sizeof(full_url), "%s?key=%s&alt=json", base_url, cfg->innertube_key);
    }

    http_header_t headers[5];
    size_t h_idx = 0;

    strncpy(headers[h_idx].name, "Authorization", sizeof(headers[h_idx].name));
    headers[h_idx].value = auth_header;
    h_idx++;

    strncpy(headers[h_idx].name, "Content-Type", sizeof(headers[h_idx].name));
    headers[h_idx].value = "application/json";
    h_idx++;

    strncpy(headers[h_idx].name, "Origin", sizeof(headers[h_idx].name));
    headers[h_idx].value = DEFAULT_SCOTTY_ORIGIN;
    h_idx++;

    strncpy(headers[h_idx].name, "X-Goog-AuthUser", sizeof(headers[h_idx].name));
    headers[h_idx].value = "0";
    h_idx++;

    strncpy(headers[h_idx].name, "Cookie", sizeof(headers[h_idx].name));
    headers[h_idx].value = (char*)cfg->cookies;
    h_idx++;

    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.url = full_url;
    req.headers = headers;
    req.header_count = h_idx;
    req.body = body_json;
    req.body_len = strlen(body_json);
    req.timeout_sec = 15;

    http_response_t resp;
    http_response_init(&resp);

    int rc = http_client_request(&req, &resp);
    free(body_json);

    if (rc != 0) {
        http_response_cleanup(&resp);
        result->http_status = 500;
        snprintf(result->error_message, sizeof(result->error_message), "Network failure during InnerTube createvideo");
        return -1;
    }

    result->http_status = resp.status_code;

    if (resp.status_code == 401) {
        snprintf(result->error_message, sizeof(result->error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        http_response_cleanup(&resp);
        return -1;
    }

    if (resp.status_code != 200 && resp.status_code != 201) {
        snprintf(result->error_message, sizeof(result->error_message), "InnerTube createvideo failed with HTTP %d", resp.status_code);
        http_response_cleanup(&resp);
        return -1;
    }

    // Extract videoId
    cJSON* resp_json = cJSON_Parse(resp.body ? resp.body : "{}");
    http_response_cleanup(&resp);

    if (!resp_json) {
        snprintf(result->error_message, sizeof(result->error_message), "Failed to parse JSON response from InnerTube");
        return -1;
    }

    cJSON* vid_item = cJSON_GetObjectItem(resp_json, "videoId");
    if (!vid_item || !cJSON_IsString(vid_item) || !vid_item->valuestring) {
        cJSON_Delete(resp_json);
        snprintf(result->error_message, sizeof(result->error_message), "Missing videoId in InnerTube response");
        return -1;
    }

    strncpy(result->video_id, vid_item->valuestring, sizeof(result->video_id) - 1);
    result->video_id[sizeof(result->video_id) - 1] = '\0';
    snprintf(result->youtube_url, sizeof(result->youtube_url), "https://youtu.be/%s", result->video_id);
    result->error_message[0] = '\0';

    cJSON_Delete(resp_json);
    return 0;
}

int youtube_upload_clip(const app_config_t* cfg, const char* filepath, const char* title, const char* description, const char* privacy, upload_progress_cb cb, void* userdata, upload_result_t* result) {
    if (!result) return -1;
    memset(result, 0, sizeof(*result));

    if (!cfg || !filepath) {
        result->http_status = 400;
        snprintf(result->error_message, sizeof(result->error_message), "Invalid arguments to youtube_upload_clip");
        return -1;
    }

    // Review Focus #2 / Issue 3: Fast failure for empty credentials
    if (!cfg->session_token[0]) {
        result->http_status = 401;
        snprintf(result->error_message, sizeof(result->error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        return -1;
    }

    char sapisid[256];
    if (extract_cookie_value(cfg->cookies, "SAPISID", sapisid, sizeof(sapisid)) != 0 &&
        extract_cookie_value(cfg->cookies, "__Secure-3PAPISID", sapisid, sizeof(sapisid)) != 0) {
        result->http_status = 401;
        snprintf(result->error_message, sizeof(result->error_message), "%s", "UNAUTHENTICATED: session token or cookies expired");
        return -1;
    }

    if (access(filepath, R_OK) != 0) {
        result->http_status = 404;
        snprintf(result->error_message, sizeof(result->error_message), "Capture file not found or unreadable: %s", filepath);
        return -1;
    }

    // Step 1: Session Init
    char frontend_id[64];
    snprintf(frontend_id, sizeof(frontend_id), "ps5_%ld_%d", (long)time(NULL), (int)(rand() % 100000));

    char upload_url[1024] = {0};
    if (youtube_init_upload_session(cfg, frontend_id, upload_url, sizeof(upload_url)) != 0) {
        result->http_status = (g_last_http_status > 0) ? g_last_http_status : 500;
        if (g_last_error_message[0]) {
            strncpy(result->error_message, g_last_error_message, sizeof(result->error_message) - 1);
        } else {
            snprintf(result->error_message, sizeof(result->error_message), "Failed to initialize upload session");
        }
        return -1;
    }

    // Step 2: Stream File
    char scotty_id[128] = {0};
    if (youtube_stream_file(cfg, upload_url, filepath, scotty_id, sizeof(scotty_id), cb, userdata) != 0) {
        result->http_status = (g_last_http_status > 0) ? g_last_http_status : 500;
        if (g_last_error_message[0]) {
            strncpy(result->error_message, g_last_error_message, sizeof(result->error_message) - 1);
        } else {
            snprintf(result->error_message, sizeof(result->error_message), "Failed to stream video bytes to Scotty");
        }
        return -1;
    }

    // Step 3: Finalize Video
    if (youtube_finalize_video(cfg, frontend_id, scotty_id, title, description, privacy, result) != 0) {
        return -1;
    }

    result->http_status = 200;
    return 0;
}
