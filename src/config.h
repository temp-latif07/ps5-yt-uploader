#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char cookies[8192];
    char session_token[512];
    char innertube_key[128];
    char channel_id[64];
    char channel_name[128];
    int auto_upload;
    char default_privacy[16]; // "PUBLIC", "UNLISTED", "PRIVATE"
    int server_port;
} app_config_t;

/**
 * Initialize configuration subsystem and persistent storage directory.
 * If base_dir is NULL or empty, defaults to "/data/youtube_uploader".
 * Recursively creates base_dir if it does not exist (mode 0777).
 * Loads or initializes the upload history database.
 *
 * @param base_dir Storage directory path
 * @return 0 on success, non-zero on failure
 */
int config_init(const char* base_dir);

/**
 * Load application configuration from config.json.
 * If config.json does not exist, populates cfg with default values.
 *
 * @param cfg Pointer to destination app_config_t
 * @return 0 on success, non-zero on error
 */
int config_load(app_config_t* cfg);

/**
 * Save application configuration to config.json.
 *
 * @param cfg Pointer to app_config_t to persist
 * @return 0 on success, non-zero on error
 */
int config_save(const app_config_t* cfg);

/**
 * Check if a capture file has already been uploaded.
 *
 * @param filepath Full path to video capture
 * @return 1 if already uploaded, 0 if not uploaded or error
 */
int history_is_uploaded(const char* filepath);

/**
 * Mark a capture file as uploaded and record its YouTube video ID.
 * Persists the update immediately to uploaded.json.
 *
 * @param filepath Full path to video capture
 * @param video_id YouTube video identifier
 * @return 0 on success, non-zero on failure
 */
int history_mark_uploaded(const char* filepath, const char* video_id);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
