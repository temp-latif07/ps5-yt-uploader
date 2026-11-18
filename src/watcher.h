#ifndef WATCHER_H
#define WATCHER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char filepath[512];
    char title_id[32];
    time_t timestamp;
    off_t size_bytes;
} capture_clip_t;

typedef void (*watcher_clip_ready_cb)(const capture_clip_t* clip, void* userdata);

/**
 * Initialize the capture watcher subsystem.
 *
 * @param captures_root Root directory to watch (e.g. "/user/av_content/video")
 * @param cb Callback invoked when a video file size has stabilized (or NULL for default upload)
 * @param userdata Pointer passed to callback
 * @return 0 on success, -1 on invalid parameter or initialization error
 */
int watcher_init(const char* captures_root, watcher_clip_ready_cb cb, void* userdata);

/**
 * Start the background file watching thread.
 *
 * @return 0 on success, non-zero on failure
 */
int watcher_start(void);

/**
 * Stop the background watching thread and wait for clean termination.
 */
void watcher_stop(void);

/**
 * Scan all existing capture clips in captures_root (e.g. /user/av_content/video/<TitleID>/)
 * and allocate an array of capture_clip_t sorted descending by timestamp (newest first).
 * The caller is responsible for calling free(*clips_out).
 *
 * @param captures_root Path to captures root directory
 * @param clips_out Pointer to receive dynamically allocated clip array
 * @param count_out Pointer to receive number of clips found
 * @return 0 on success, -1 on failure
 */
int scan_existing_clips(const char* captures_root, capture_clip_t** clips_out, size_t* count_out);

/**
 * Set the polling interval for the watcher background thread in milliseconds.
 * Default is 2000 ms.
 *
 * @param interval_ms Interval in milliseconds
 */
void watcher_set_poll_interval_ms(int interval_ms);

/**
 * Execute a single scan iteration synchronously.
 * Checks for newly created or growing clips, performs write-debouncing,
 * and triggers callback or auto-upload for clips that have stabilized.
 *
 * @return Number of stabilized clips detected in this scan, or -1 on error
 */
int watcher_scan_once(void);

/**
 * Upload a captured clip using current app configuration and send system notifications.
 *
 * @param clip Pointer to capture_clip_t
 * @return 0 on success, -1 on failure
 */
int watcher_process_clip_upload(const capture_clip_t* clip);

#ifdef __cplusplus
}
#endif

#endif /* WATCHER_H */
