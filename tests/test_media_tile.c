#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sqlite3.h>
#include "media_tile.h"

#define TEST_DB_PATH "/tmp/test_ps5_app.db"
#define TEST_DB_EMPTY_PATH "/tmp/test_ps5_app_empty.db"
#define TEST_DB_LEGACY_PATH "/tmp/test_ps5_app_legacy.db"

static void cleanup_files(void) {
    unlink(TEST_DB_PATH);
    unlink(TEST_DB_EMPTY_PATH);
    unlink(TEST_DB_LEGACY_PATH);
}

static void test_media_tile_register_and_idempotency(void) {
    printf("[Test 1] Testing media_tile_register on mock app.db...\n");
    unlink(TEST_DB_PATH);

    /* 1. Setup mock SQLite database with PS5 schema */
    sqlite3* db = NULL;
    int rc = sqlite3_open(TEST_DB_PATH, &db);
    assert(rc == SQLITE_OK && db != NULL);

    const char* create_sql =
        "CREATE TABLE tbl_appbrowse_0123456789 ("
        "  titleId TEXT PRIMARY KEY,"
        "  titleName TEXT,"
        "  category INTEGER,"
        "  url TEXT,"
        "  iconPath TEXT,"
        "  mtime INTEGER"
        ");";
    char* err_msg = NULL;
    rc = sqlite3_exec(db, create_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite exec error: %s\n", err_msg);
        sqlite3_free(err_msg);
        assert(0);
    }
    sqlite3_close(db);

    /* 2. Call media_tile_register */
    rc = media_tile_register(TEST_DB_PATH, "YouTube Uploader", "http://127.0.0.1:8888");
    assert(rc == 0);

    /* 3. Verify entry exists with category 2, title, URL, iconPath, and mtime */
    rc = sqlite3_open(TEST_DB_PATH, &db);
    assert(rc == SQLITE_OK);

    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT titleId, titleName, category, url, iconPath, mtime FROM tbl_appbrowse_0123456789 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);

    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);

    const char* title_id = (const char*)sqlite3_column_text(stmt, 0);
    const char* title_name = (const char*)sqlite3_column_text(stmt, 1);
    int category = sqlite3_column_int(stmt, 2);
    const char* url = (const char*)sqlite3_column_text(stmt, 3);
    const char* icon_path = (const char*)sqlite3_column_text(stmt, 4);
    sqlite3_int64 mtime = sqlite3_column_int64(stmt, 5);

    assert(strcmp(title_id, "YOUTUBE00") == 0);
    assert(strcmp(title_name, "YouTube Uploader") == 0);
    assert(category == 2);
    assert(strcmp(url, "http://127.0.0.1:8888") == 0);
    assert(strstr(icon_path, "icon.png") != NULL);
    assert(mtime > 0);

    /* Ensure exactly one row exists */
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    printf("  -> Registered tile verified with category 2, title, and URL!\n");

    /* 4. Test idempotency: call media_tile_register second time with updated URL */
    printf("[Test 2] Testing idempotency and URL update without duplicates...\n");
    rc = media_tile_register(TEST_DB_PATH, "YouTube Uploader", "http://127.0.0.1:8888/updated");
    assert(rc == 0);

    rc = sqlite3_prepare_v2(db, "SELECT titleName, category, url FROM tbl_appbrowse_0123456789 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "YouTube Uploader") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 2), "http://127.0.0.1:8888/updated") == 0);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Verify row count is still exactly 1 */
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tbl_appbrowse_0123456789 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  -> Idempotent update verified with 0 duplicates!\n");
}

static void test_media_tile_unregister(void) {
    printf("[Test 3] Testing media_tile_unregister...\n");
    /* Test DB already has YOUTUBE00 from Test 1/2 */
    int rc = media_tile_unregister(TEST_DB_PATH, "YOUTUBE00");
    assert(rc == 0);

    sqlite3* db = NULL;
    rc = sqlite3_open(TEST_DB_PATH, &db);
    assert(rc == SQLITE_OK);

    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tbl_appbrowse_0123456789 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Calling unregister again when row is absent should succeed gracefully */
    rc = media_tile_unregister(TEST_DB_PATH, "YOUTUBE00");
    assert(rc == 0);

    /* Calling with NULL title_id defaults to YOUTUBE00 */
    rc = media_tile_unregister(TEST_DB_PATH, NULL);
    assert(rc == 0);

    printf("  -> Clean unregister verified!\n");
}

static void test_media_tile_multiple_user_tables(void) {
    printf("[Test 4] Testing media_tile across multiple user profiles (tbl_appbrowse_%%)...\n");
    unlink(TEST_DB_PATH);

    sqlite3* db = NULL;
    int rc = sqlite3_open(TEST_DB_PATH, &db);
    assert(rc == SQLITE_OK);

    const char* schema =
        "CREATE TABLE tbl_appbrowse_user001 (titleId TEXT PRIMARY KEY, titleName TEXT, category INTEGER, url TEXT, iconPath TEXT, mtime INTEGER);"
        "CREATE TABLE tbl_appbrowse_user002 (titleId TEXT PRIMARY KEY, titleName TEXT, category INTEGER, url TEXT, iconPath TEXT, mtime INTEGER);"
        "CREATE TABLE tbl_other_table (id INTEGER PRIMARY KEY, name TEXT);";

    char* err_msg = NULL;
    rc = sqlite3_exec(db, schema, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite exec error: %s\n", err_msg);
        sqlite3_free(err_msg);
        assert(0);
    }
    sqlite3_close(db);

    /* Register tile */
    rc = media_tile_register(TEST_DB_PATH, "PS5 YouTube", "http://127.0.0.1:8888");
    assert(rc == 0);

    rc = sqlite3_open(TEST_DB_PATH, &db);
    assert(rc == SQLITE_OK);

    /* Check tbl_appbrowse_user001 */
    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT titleName, category, url FROM tbl_appbrowse_user001 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "PS5 YouTube") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 2), "http://127.0.0.1:8888") == 0);
    sqlite3_finalize(stmt);

    /* Check tbl_appbrowse_user002 */
    rc = sqlite3_prepare_v2(db, "SELECT titleName, category, url FROM tbl_appbrowse_user002 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "PS5 YouTube") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 2), "http://127.0.0.1:8888") == 0);
    sqlite3_finalize(stmt);

    /* Check tbl_other_table unaffected */
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tbl_other_table;", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    sqlite3_close(db);

    /* Unregister from all tables */
    rc = media_tile_unregister(TEST_DB_PATH, "YOUTUBE00");
    assert(rc == 0);

    rc = sqlite3_open(TEST_DB_PATH, &db);
    assert(rc == SQLITE_OK);
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tbl_appbrowse_user001 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tbl_appbrowse_user002 WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    printf("  -> Multi-profile registration and unregistration verified!\n");
}

static void test_media_tile_auto_table_creation(void) {
    printf("[Test 5] Testing auto-creation of table when none exist...\n");
    unlink(TEST_DB_EMPTY_PATH);

    /* Call register on empty database file */
    int rc = media_tile_register(TEST_DB_EMPTY_PATH, "Default App", "http://127.0.0.1:8888");
    assert(rc == 0);

    sqlite3* db = NULL;
    rc = sqlite3_open(TEST_DB_EMPTY_PATH, &db);
    assert(rc == SQLITE_OK);

    /* Verify a tbl_appbrowse_% table was created and has the row */
    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_appbrowse_%';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char* table_name = (const char*)sqlite3_column_text(stmt, 0);
    assert(table_name != NULL && strlen(table_name) > 0);
    char query[256];
    snprintf(query, sizeof(query), "SELECT titleName, category, url FROM \"%s\" WHERE titleId = 'YOUTUBE00';", table_name);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "Default App") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 2), "http://127.0.0.1:8888") == 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    printf("  -> Auto-creation of table verified!\n");
}

static void test_media_tile_missing_columns_migration(void) {
    printf("[Test 6] Testing dynamic column migration for legacy tables...\n");
    unlink(TEST_DB_LEGACY_PATH);

    sqlite3* db = NULL;
    int rc = sqlite3_open(TEST_DB_LEGACY_PATH, &db);
    assert(rc == SQLITE_OK);

    /* Table only has titleId and titleName, missing category, url, iconPath, mtime */
    const char* legacy_schema =
        "CREATE TABLE tbl_appbrowse_legacy (titleId TEXT PRIMARY KEY, titleName TEXT);";
    char* err_msg = NULL;
    rc = sqlite3_exec(db, legacy_schema, NULL, NULL, &err_msg);
    assert(rc == SQLITE_OK);
    sqlite3_close(db);

    rc = media_tile_register(TEST_DB_LEGACY_PATH, "Migrated App", "http://127.0.0.1:8888/migrated");
    assert(rc == 0);

    rc = sqlite3_open(TEST_DB_LEGACY_PATH, &db);
    assert(rc == SQLITE_OK);

    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT titleName, category, url, iconPath, mtime FROM tbl_appbrowse_legacy WHERE titleId = 'YOUTUBE00';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "Migrated App") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 2), "http://127.0.0.1:8888/migrated") == 0);
    assert(sqlite3_column_text(stmt, 3) != NULL);
    assert(sqlite3_column_int64(stmt, 4) > 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    printf("  -> Legacy column migration verified!\n");
}

static void test_media_tile_browser_launch(void) {
    printf("[Test 7] Testing media_tile_launch_browser and URL security validation...\n");
    int rc = media_tile_launch_browser(NULL);
    assert(rc == 0);

    rc = media_tile_launch_browser("http://127.0.0.1:8888");
    assert(rc == 0);

    rc = media_tile_launch_browser("https://127.0.0.1:8888/dashboard");
    assert(rc == 0);

    /* Invalid scheme rejection */
    assert(media_tile_launch_browser("ftp://127.0.0.1:8888") == -1);
    assert(media_tile_launch_browser("javascript:alert(1)") == -1);
    assert(media_tile_launch_browser("file:///etc/passwd") == -1);

    /* Shell injection rejection */
    assert(media_tile_launch_browser("http://127.0.0.1:8888\"; rm -rf /") == -1);
    assert(media_tile_launch_browser("http://127.0.0.1:8888 && reboot") == -1);
    assert(media_tile_launch_browser("http://127.0.0.1:8888`id`") == -1);
    assert(media_tile_launch_browser("http://127.0.0.1:8888$(whoami)") == -1);
    assert(media_tile_launch_browser("http://127.0.0.1:8888 | ls") == -1);

    printf("  -> Browser launch mock and shell security validation verified!\n");
}

int main(void) {
    cleanup_files();

    test_media_tile_register_and_idempotency();
    test_media_tile_unregister();
    test_media_tile_multiple_user_tables();
    test_media_tile_auto_table_creation();
    test_media_tile_missing_columns_migration();
    test_media_tile_browser_launch();

    cleanup_files();
    printf("Task 7 Media Tile Tests Passed Successfully!\n");
    return 0;
}
