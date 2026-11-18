#include "media_tile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef PS5
#include <sqlite3.h>
#endif

#if defined(PS5)
#include "shortcut_assets.h"
#include "notify.h"

#define PS5_TITLE_ID "PS5YT0001"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* dir, void* reserved);
int sceAppInstUtilAppInstallAll(void* reserved);
int sceUserServiceInitialize(void* reserved);
int sceSystemServiceLaunchWebBrowser(const char* uri, void* reserved);

static int ps5_mkdir_p(const char* path, mode_t mode) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ps5_install_file(const char* path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int ps5_file_needs_update(const char* path, const uint8_t* expected_data, size_t expected_size) {
    struct stat st;
    if (stat(path, &st) != 0) return 1;
    if ((size_t)st.st_size != expected_size) return 1;
    FILE* f = fopen(path, "rb");
    if (!f) return 1;
    uint8_t* buf = (uint8_t*)malloc(expected_size);
    if (!buf) {
        fclose(f);
        return 1;
    }
    size_t read_bytes = fread(buf, 1, expected_size, f);
    fclose(f);
    if (read_bytes != expected_size) {
        free(buf);
        return 1;
    }
    int diff = memcmp(buf, expected_data, expected_size);
    free(buf);
    return diff != 0;
}

int media_tile_register(const char* db_path, const char* app_title, const char* launch_url) {
    (void)db_path;
    (void)app_title;
    (void)launch_url;

    const char* title_id = PS5_TITLE_ID;
    char base_dir[256];
    char sce_sys_dir[256];
    char param_path[256];
    char icon_path[256];

    snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "/user/app/%s/sce_sys", title_id);
    snprintf(param_path, sizeof(param_path), "/user/app/%s/sce_sys/param.json", title_id);
    snprintf(icon_path, sizeof(icon_path), "/user/app/%s/sce_sys/icon0.png", title_id);

    int update_needed = 0;
    struct stat st;
    if (stat(base_dir, &st) != 0) {
        update_needed = 1;
    } else {
        if (ps5_file_needs_update(param_path, g_param_json, g_param_json_size)) {
            update_needed = 1;
        }
        if (ps5_file_needs_update(icon_path, g_icon0_png, g_icon0_png_size)) {
            update_needed = 1;
        }
    }

    if (!update_needed) {
        printf("[TILE] Tile %s is already installed and up to date\n", title_id);
        return 0;
    }

    printf("[TILE] Installing PS5 home screen tile (%s)...\n", title_id);
    notify_send("Installing YouTube Uploader Tile...");

    if (sceAppInstUtilInitialize() != 0) {
        printf("[TILE] Warning: sceAppInstUtilInitialize failed\n");
    }

    if (ps5_mkdir_p(sce_sys_dir, 0755) != 0) {
        printf("[TILE] Failed to create app dir: %s (errno: %d)\n", sce_sys_dir, errno);
        sceAppInstUtilTerminate();
        return -1;
    }

    if (ps5_install_file(param_path, g_param_json, g_param_json_size) != 0) {
        printf("[TILE] Failed to install param.json\n");
        sceAppInstUtilTerminate();
        return -1;
    }

    if (ps5_install_file(icon_path, g_icon0_png, g_icon0_png_size) != 0) {
        printf("[TILE] Failed to install icon0.png\n");
        sceAppInstUtilTerminate();
        return -1;
    }

    int rc = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", NULL);
    if (rc != 0) {
        printf("[TILE] sceAppInstUtilAppInstallTitleDir returned 0x%08x, falling back to sceAppInstUtilAppInstallAll\n", rc);
        rc = sceAppInstUtilAppInstallAll(NULL);
    }

    sceAppInstUtilTerminate();

    if (rc == 0) {
        printf("[TILE] PS5 home screen tile %s registered successfully.\n", title_id);
        notify_send("YouTube Uploader Tile Installed!");
    } else {
        printf("[TILE] App registration returned 0x%08x\n", rc);
    }

    return 0;
}

int media_tile_launch_browser(const char* launch_url) {
    if (!launch_url || !*launch_url) {
        return -1;
    }
    printf("[TILE] Launching PS5 browser: %s\n", launch_url);
    sceUserServiceInitialize(NULL);
    int rc = sceSystemServiceLaunchWebBrowser(launch_url, NULL);
    if (rc != 0) {
        printf("[TILE] Failed to launch PS5 browser (error: 0x%08x)\n", rc);
        return -1;
    }
    return 0;
}

int media_tile_unregister(const char* db_path, const char* title_id) {
    (void)db_path;
    (void)title_id;
    return 0;
}
#else

#define MAX_TABLES 64
#define TABLE_NAME_MAX 128

/**
 * Creates parent directories recursively.
 */
static int mkdir_p_for_file(const char* filepath) {
    if (!filepath || !*filepath) {
        return -1;
    }

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    char* slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        return 0; /* Current directory or root */
    }
    *slash = '\0';

    char* p = NULL;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/**
 * Validates table name to prevent SQL injection when formatting identifier.
 * Accepts only alphanumeric characters and underscores, starting with 'tbl_appbrowse_'.
 */
static int is_valid_table_name(const char* name) {
    if (!name) return 0;
    const char* prefix = "tbl_appbrowse_";
    size_t prefix_len = strlen(prefix);
    if (strncmp(name, prefix, prefix_len) != 0) {
        return 0;
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_') {
            return 0;
        }
    }
    return 1;
}

/**
 * Validates that launch URL starts with http:// or https:// and contains no shell metacharacters.
 */
static int is_valid_launch_url(const char* url) {
    if (!url) return 0;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return 0;
    }
    for (const char* p = url; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 32 || c >= 127) {
            return 0; /* No whitespace, control chars, or non-ASCII */
        }
        if (c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' ||
            c == ';' || c == '&'  || c == '|' || c == '<' || c == '>'  ||
            c == '(' || c == ')'  || c == '{' || c == '}' || c == '*'  ||
            c == '?' || c == '['  || c == ']' || c == '^' || c == '!') {
            return 0;
        }
    }
    return 1;
}

/**
 * Ensures required columns exist in the target table (for legacy database schemas).
 */
static int ensure_table_columns(sqlite3* db, const char* tbl_name) {
    char query[256];
    snprintf(query, sizeof(query), "PRAGMA table_info(\"%s\");", tbl_name);

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }

    int has_category = 0;
    int has_url = 0;
    int has_icon_path = 0;
    int has_mtime = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* col_name = (const char*)sqlite3_column_text(stmt, 1);
        if (col_name) {
            if (strcmp(col_name, "category") == 0) has_category = 1;
            else if (strcmp(col_name, "url") == 0) has_url = 1;
            else if (strcmp(col_name, "iconPath") == 0) has_icon_path = 1;
            else if (strcmp(col_name, "mtime") == 0) has_mtime = 1;
        }
    }
    sqlite3_finalize(stmt);

    char alter_sql[512];
    if (!has_category) {
        snprintf(alter_sql, sizeof(alter_sql), "ALTER TABLE \"%s\" ADD COLUMN category INTEGER DEFAULT 0;", tbl_name);
        if (sqlite3_exec(db, alter_sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
    }
    if (!has_url) {
        snprintf(alter_sql, sizeof(alter_sql), "ALTER TABLE \"%s\" ADD COLUMN url TEXT;", tbl_name);
        if (sqlite3_exec(db, alter_sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
    }
    if (!has_icon_path) {
        snprintf(alter_sql, sizeof(alter_sql), "ALTER TABLE \"%s\" ADD COLUMN iconPath TEXT;", tbl_name);
        if (sqlite3_exec(db, alter_sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
    }
    if (!has_mtime) {
        snprintf(alter_sql, sizeof(alter_sql), "ALTER TABLE \"%s\" ADD COLUMN mtime INTEGER DEFAULT 0;", tbl_name);
        if (sqlite3_exec(db, alter_sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
    }

    return 0;
}

int media_tile_register(const char* db_path, const char* app_title, const char* launch_url) {
    const char* path = (db_path && *db_path) ? db_path : MEDIA_TILE_DEFAULT_DB_PATH;
    const char* title = (app_title && *app_title) ? app_title : MEDIA_TILE_DEFAULT_TITLE_NAME;
    const char* url = (launch_url && *launch_url) ? launch_url : MEDIA_TILE_DEFAULT_LAUNCH_URL;
    const char* title_id = MEDIA_TILE_DEFAULT_TITLE_ID;
    const char* icon_path = MEDIA_TILE_DEFAULT_ICON_PATH;
    int category = MEDIA_TILE_CATEGORY_MEDIA;
    time_t now = time(NULL);

    if (mkdir_p_for_file(path) != 0) {
        /* Non-fatal: path might be in existing root dir */
    }

    sqlite3* db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);

    /* Find all matching tbl_appbrowse_% tables */
    const char* find_tables_sql =
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_appbrowse_%';";
    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, find_tables_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    char tables[MAX_TABLES][TABLE_NAME_MAX];
    int table_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && table_count < MAX_TABLES) {
        const char* name = (const char*)sqlite3_column_text(stmt, 0);
        if (name && strlen(name) < TABLE_NAME_MAX && is_valid_table_name(name)) {
            snprintf(tables[table_count], sizeof(tables[table_count]), "%s", name);
            table_count++;
        }
    }
    sqlite3_finalize(stmt);

    /* If no tables found, create default table */
    if (table_count == 0) {
        const char* default_table = "tbl_appbrowse_0000000000";
        char create_sql[512];
        snprintf(create_sql, sizeof(create_sql),
            "CREATE TABLE IF NOT EXISTS \"%s\" ("
            "  titleId TEXT PRIMARY KEY,"
            "  titleName TEXT,"
            "  category INTEGER,"
            "  url TEXT,"
            "  iconPath TEXT,"
            "  mtime INTEGER"
            ");", default_table);

        rc = sqlite3_exec(db, create_sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_close(db);
            return -1;
        }

        snprintf(tables[0], sizeof(tables[0]), "%s", default_table);
        table_count = 1;
    }

    /* Begin atomic transaction */
    rc = sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    for (int i = 0; i < table_count; i++) {
        const char* tbl_name = tables[i];

        if (ensure_table_columns(db, tbl_name) != 0) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return -1;
        }

        /* 1. Try updating existing row */
        char update_sql[512];
        snprintf(update_sql, sizeof(update_sql),
            "UPDATE \"%s\" SET titleName = ?, category = ?, url = ?, iconPath = ?, mtime = ? WHERE titleId = ?;",
            tbl_name);

        sqlite3_stmt* update_stmt = NULL;
        rc = sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return -1;
        }

        sqlite3_bind_text(update_stmt, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int(update_stmt, 2, category);
        sqlite3_bind_text(update_stmt, 3, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(update_stmt, 4, icon_path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(update_stmt, 5, (sqlite3_int64)now);
        sqlite3_bind_text(update_stmt, 6, title_id, -1, SQLITE_STATIC);

        rc = sqlite3_step(update_stmt);
        sqlite3_finalize(update_stmt);

        if (rc != SQLITE_DONE) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return -1;
        }

        /* 2. If row did not exist, insert it */
        if (sqlite3_changes(db) == 0) {
            char insert_sql[512];
            snprintf(insert_sql, sizeof(insert_sql),
                "INSERT INTO \"%s\" (titleId, titleName, category, url, iconPath, mtime) VALUES (?, ?, ?, ?, ?, ?);",
                tbl_name);

            sqlite3_stmt* insert_stmt = NULL;
            rc = sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, NULL);
            if (rc != SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                sqlite3_close(db);
                return -1;
            }

            sqlite3_bind_text(insert_stmt, 1, title_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_stmt, 2, title, -1, SQLITE_STATIC);
            sqlite3_bind_int(insert_stmt, 3, category);
            sqlite3_bind_text(insert_stmt, 4, url, -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_stmt, 5, icon_path, -1, SQLITE_STATIC);
            sqlite3_bind_int64(insert_stmt, 6, (sqlite3_int64)now);

            rc = sqlite3_step(insert_stmt);
            sqlite3_finalize(insert_stmt);

            if (rc != SQLITE_DONE) {
                sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                sqlite3_close(db);
                return -1;
            }
        }
    }

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);

    return (rc == SQLITE_OK) ? 0 : -1;
}

int media_tile_unregister(const char* db_path, const char* title_id) {
    const char* path = (db_path && *db_path) ? db_path : MEDIA_TILE_DEFAULT_DB_PATH;
    const char* tid = (title_id && *title_id) ? title_id : MEDIA_TILE_DEFAULT_TITLE_ID;

    sqlite3* db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);

    /* Find all matching tbl_appbrowse_% tables */
    const char* find_tables_sql =
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_appbrowse_%';";
    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, find_tables_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    char tables[MAX_TABLES][TABLE_NAME_MAX];
    int table_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && table_count < MAX_TABLES) {
        const char* name = (const char*)sqlite3_column_text(stmt, 0);
        if (name && strlen(name) < TABLE_NAME_MAX && is_valid_table_name(name)) {
            snprintf(tables[table_count], sizeof(tables[table_count]), "%s", name);
            table_count++;
        }
    }
    sqlite3_finalize(stmt);

    if (table_count == 0) {
        sqlite3_close(db);
        return 0;
    }

    rc = sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    for (int i = 0; i < table_count; i++) {
        char delete_sql[256];
        snprintf(delete_sql, sizeof(delete_sql), "DELETE FROM \"%s\" WHERE titleId = ?;", tables[i]);

        sqlite3_stmt* del_stmt = NULL;
        rc = sqlite3_prepare_v2(db, delete_sql, -1, &del_stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return -1;
        }

        sqlite3_bind_text(del_stmt, 1, tid, -1, SQLITE_STATIC);
        rc = sqlite3_step(del_stmt);
        sqlite3_finalize(del_stmt);

        if (rc != SQLITE_DONE) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return -1;
        }
    }

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);

    return (rc == SQLITE_OK) ? 0 : -1;
}

int media_tile_launch_browser(const char* launch_url) {
    const char* url = (launch_url && *launch_url) ? launch_url : MEDIA_TILE_DEFAULT_LAUNCH_URL;

    if (!is_valid_launch_url(url)) {
        return -1;
    }

#if defined(__FreeBSD__) || defined(PS5)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ps5-web-browser-fullscreen \"%s\" &", url);
    return system(cmd);
#else
    return 0;
#endif
}
#endif /* !PS5 */
