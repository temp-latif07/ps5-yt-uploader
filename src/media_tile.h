#ifndef MEDIA_TILE_H
#define MEDIA_TILE_H

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_TILE_DEFAULT_DB_PATH "/system_data/priv/mms/app.db"
#define MEDIA_TILE_DEFAULT_TITLE_ID "YOUTUBE00"
#define MEDIA_TILE_DEFAULT_TITLE_NAME "YouTube Uploader"
#define MEDIA_TILE_DEFAULT_LAUNCH_URL "http://127.0.0.1:8888"
#define MEDIA_TILE_DEFAULT_ICON_PATH "/data/youtube_uploader/icon.png"
#define MEDIA_TILE_CATEGORY_MEDIA 2

/**
 * Register or update the media category launcher tile in the PS5 MMS database.
 *
 * Scans sqlite_master for all user application browse tables (tbl_appbrowse_%)
 * or creates a default table if none exist.
 * Inserts or updates the row for MEDIA_TILE_DEFAULT_TITLE_ID ("YOUTUBE00")
 * with category 2 (Media category), the specified app_title, launch_url,
 * default iconPath, and updated timestamp.
 * Operation is idempotent and safe against duplicate entries.
 *
 * @param db_path Path to MMS SQLite database (NULL for MEDIA_TILE_DEFAULT_DB_PATH)
 * @param app_title Display title for the tile (NULL for MEDIA_TILE_DEFAULT_TITLE_NAME)
 * @param launch_url Fullscreen WebKit launch URL (NULL for MEDIA_TILE_DEFAULT_LAUNCH_URL)
 * @return 0 on success, -1 on failure
 */
int media_tile_register(const char* db_path, const char* app_title, const char* launch_url);

/**
 * Unregister/remove the media launcher tile from the PS5 MMS database.
 *
 * Deletes any records matching title_id across all tbl_appbrowse_% tables.
 *
 * @param db_path Path to MMS SQLite database (NULL for MEDIA_TILE_DEFAULT_DB_PATH)
 * @param title_id Title ID to remove (NULL for MEDIA_TILE_DEFAULT_TITLE_ID)
 * @return 0 on success, -1 on failure
 */
int media_tile_unregister(const char* db_path, const char* title_id);

/**
 * Launch the fullscreen WebKit browser pointing to launch_url.
 * On PS5, executes "ps5-web-browser-fullscreen <url>".
 * On host platforms, executes platform browser or acts as a safe mock.
 *
 * @param launch_url URL to open (NULL for MEDIA_TILE_DEFAULT_LAUNCH_URL)
 * @return 0 on success, -1 on failure
 */
int media_tile_launch_browser(const char* launch_url);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_TILE_H */
