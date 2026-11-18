#ifndef PS5_YOUTUBE_H
#define PS5_YOUTUBE_H

#include <stddef.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char video_id[32];
    char youtube_url[128];
    int http_status;
    char error_message[256];
} upload_result_t;

typedef void (*upload_progress_cb)(size_t uploaded_bytes, size_t total_bytes, void* userdata);

/**
 * Override default endpoint URLs for local mock testing or custom proxies.
 * Passing NULL reverts to standard YouTube endpoints.
 */
void youtube_set_base_urls(const char* scotty_init_url, const char* innertube_createvideo_url);

/**
 * Get the Scotty streaming chunk size (default: 8 MB).
 */
size_t youtube_get_chunk_size(void);

/**
 * Set the Scotty streaming chunk size in bytes.
 */
void youtube_set_chunk_size(size_t chunk_size);

/**
 * Step 1: Initialize a Scotty resumable upload session.
 * Makes a POST request to Scotty Init endpoint and extracts x-goog-upload-url header.
 *
 * @param cfg             Application configuration containing cookies and session token
 * @param frontend_id     Unique identifier for this upload session
 * @param upload_url_out  Buffer receiving the allocated resumable upload URL
 * @param max_len         Capacity of upload_url_out
 * @return 0 on success, -1 on failure
 */
int youtube_init_upload_session(const app_config_t* cfg, const char* frontend_id, char* upload_url_out, size_t max_len);

/**
 * Step 2: Stream video file to Scotty upload URL.
 * Handles network disconnection and resumes stream using Scotty query commands.
 *
 * @param cfg            Application configuration
 * @param upload_url     Scotty upload URL obtained from session initialization
 * @param filepath       Full path to video file on disk
 * @param scotty_id_out  Buffer receiving scottyResourceId
 * @param max_len        Capacity of scotty_id_out
 * @param cb             Optional progress callback
 * @param userdata       User pointer passed to progress callback
 * @return 0 on success, -1 on failure
 */
int youtube_stream_file(const app_config_t* cfg, const char* upload_url, const char* filepath, char* scotty_id_out, size_t max_len, upload_progress_cb cb, void* userdata);

/**
 * Step 3: Finalize video creation via InnerTube createvideo endpoint.
 *
 * @param cfg          Application configuration
 * @param frontend_id  Unique upload identifier
 * @param scotty_id    scottyResourceId from streaming phase
 * @param title        Video title
 * @param description  Video description
 * @param privacy      "PUBLIC", "UNLISTED", or "PRIVATE"
 * @param result       Result structure receiving video ID, URL, status, and error
 * @return 0 on success, -1 on failure
 */
int youtube_finalize_video(const app_config_t* cfg, const char* frontend_id, const char* scotty_id, const char* title, const char* description, const char* privacy, upload_result_t* result);

/**
 * High-level unified upload pipeline:
 * Executes Init -> Stream -> Finalize.
 *
 * @param cfg          Application configuration
 * @param filepath     Full path to video file on disk
 * @param title        Video title
 * @param description  Video description
 * @param privacy      "PUBLIC", "UNLISTED", or "PRIVATE"
 * @param cb           Optional progress callback
 * @param userdata     User pointer passed to progress callback
 * @param result       Result structure receiving video ID, URL, status, and error
 * @return 0 on success, -1 on failure
 */
int youtube_upload_clip(const app_config_t* cfg, const char* filepath, const char* title, const char* description, const char* privacy, upload_progress_cb cb, void* userdata, upload_result_t* result);

#ifdef __cplusplus
}
#endif

#endif /* PS5_YOUTUBE_H */
