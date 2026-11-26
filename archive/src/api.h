#ifndef API_H
#define API_H

#ifdef __cplusplus
extern "C" {
#endif

#define API_MAX_CONCURRENT_UPLOADS 4

/**
 * Initialize REST API routes and set captures root directory for clip discovery.
 * Registers routes:
 *   GET  /api/status
 *   GET  /api/clips
 *   POST /api/upload
 *   POST /api/auth
 *   POST /api/config
 *
 * @param captures_root Path to captures root directory (e.g. "/user/av_content/video")
 * @return 0 on success, -1 on failure
 */
int api_init(const char* captures_root);

/**
 * Clean up API subsystem and wait for any pending background upload tasks.
 */
void api_cleanup(void);

/**
 * Set an optional artificial delay in milliseconds for upload worker execution (used in tests).
 */
void api_set_upload_delay_ms(int ms);

/**
 * Get the current number of active background upload workers.
 */
int api_get_active_uploads(void);

#ifdef __cplusplus
}
#endif

#endif /* API_H */
