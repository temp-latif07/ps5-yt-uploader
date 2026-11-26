// Unit tests for core utilities, SHA-1, JSON, path checks, and cookie/session management.
#include "app.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const unsigned char cacert_pem[] = "";
const size_t cacert_pem_len = 0;
void api_handle(http_req *req) { (void)req; }

static void test_sha1(void) {
    char hex[41];
    sha1_hex((const unsigned char *)"abc", 3, hex);
    assert(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"));

    sha1_hex((const unsigned char *)"", 0, hex);
    assert(!strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));

    char hash[64];
    assert(sapisid_hash("my_sapisid", "https://studio.youtube.com", 1700000000L, hash, sizeof hash) == 0);
    assert(!strncmp(hash, "1700000000_", 11));
    assert(strlen(hash) == 11 + 40);
    printf("ok   sha1 & sapisid_hash\n");
}

static void test_json(void) {
    const char *doc = "{\"ok\":true,\"count\":42,\"msg\":\"hello \\\"world\\\"\\n\\u0041\",\"arr\":[10,{\"k\":\"v\"}],\"flag\":false}";
    assert(json_bool(json_find(doc, "ok")) == 1);
    assert(json_bool(json_find(doc, "flag")) == 0);
    assert(json_int(json_find(doc, "count"), -1) == 42);
    char buf[64];
    assert(json_string(json_find(doc, "msg"), buf, sizeof buf) == 0);
    assert(!strcmp(buf, "hello \"world\"\nA"));
    const char *e1 = json_array_first(json_find(doc, "arr"));
    assert(json_int(e1, 0) == 10);
    const char *e2 = json_array_next(e1);
    assert(json_string(json_find(e2, "k"), buf, sizeof buf) == 0 && !strcmp(buf, "v"));
    assert(json_array_next(e2) == NULL);
    printf("ok   json parser\n");
}

static void test_form_and_util(void) {
    char out[64];
    const char *form = "title=My+Clip%21&privacy=PUBLIC&empty=&big=abcdef";
    assert(form_get(form, "title", out, sizeof out) != NULL && !strcmp(out, "My Clip!"));
    assert(form_get(form, "privacy", out, sizeof out) != NULL && !strcmp(out, "PUBLIC"));
    assert(form_get(form, "empty", out, sizeof out) != NULL && !strcmp(out, ""));
    assert(form_get(form, "missing", out, sizeof out) == NULL);

    char *alloc = form_get_alloc(form, "big", 1024);
    assert(alloc && !strcmp(alloc, "abcdef"));
    free(alloc);

    assert(is_video_filename("clip.mp4") == 1);
    assert(is_video_filename("/user/av_content/video/PPSA01234/TEST.MP4") == 1);
    assert(is_video_filename("clip.webm") == 1);
    assert(is_video_filename(".hidden.mp4") == 0);
    assert(is_video_filename(".mp4") == 0);
    assert(is_video_filename("cookies.txt") == 0);
    assert(is_video_filename("/data/ps5-yt-uploader/cookies.txt") == 0);
    printf("ok   form & video filename helpers\n");
}

static void test_paths(void) {
    setenv("PS5YT_APP_DIR", "/tmp/ps5yt_unit_app", 1);
    setenv("PS5YT_CAPTURES_DIR", "/tmp/ps5yt_unit_cap", 1);
    setenv("PS5YT_ROOTS", "/tmp/ps5yt_unit_root:/var/tmp/ps5yt_other", 1);
    plat_init();

    assert(plat_path_allowed("/tmp/ps5yt_unit_cap") == 1);
    assert(plat_path_allowed("/tmp/ps5yt_unit_cap/PPSA01234/clip.mp4") == 1);
    assert(plat_path_allowed("/tmp/ps5yt_unit_root/sub") == 1);
    assert(plat_path_allowed("/var/tmp/ps5yt_other/clip.webm") == 1);
    assert(plat_path_allowed("/etc/passwd") == 0);
    assert(plat_path_allowed("/tmp/ps5yt_unit_cap/../etc/passwd") == 0);
    assert(plat_path_allowed("/tmp/ps5yt_unit_cap/..") == 0);
    assert(plat_path_allowed("/tmp/ps5yt_unit_cap/.") == 0);
    assert(plat_path_allowed("relative/clip.mp4") == 0);
    printf("ok   platform allowed paths\n");
}

static void test_cookies_and_session(void) {
    setenv("PS5YT_APP_DIR", "/tmp/ps5yt_unit_net", 1);
    unsetenv("PS5YT_STUDIO_URL");
    plat_init();
    net_init();
    net_auth_clear();

    sbuf jar;
    char err[256] = "";
    assert(net_normalize_cookies("SID=abc; HSID=xyz", &jar, err, sizeof err) == -1);
    assert(strstr(err, "SAPISID") != NULL);

    assert(net_normalize_cookies("Cookie: SID=s123; SAPISID=sap_raw; LOGIN_INFO=li", &jar, err, sizeof err) == 0);
    assert(strstr(jar.data, ".youtube.com\tTRUE\t/\tTRUE\t2147483647\tSAPISID\tsap_raw") != NULL);
    sb_free(&jar);

    const char *netscape_in =
        "# Netscape HTTP Cookie File\n"
        ".example.com\tTRUE\t/\tFALSE\t2147483647\tIGNORE\tbad\n"
        "#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t2147483647\tSID\tsid_val\n"
        ".youtube.com\tTRUE\t/\tTRUE\t2147483647\tSAPISID\tsap_net\n";
    assert(net_normalize_cookies(netscape_in, &jar, err, sizeof err) == 0);
    assert(strstr(jar.data, "IGNORE") == NULL);
    assert(strstr(jar.data, "#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t2147483647\tSID\tsid_val") != NULL);

    char tmp[600], snap[600], val[128];
    assert(net_write_temp_jar(jar.data, tmp, sizeof tmp) == 0);
    sb_free(&jar);
    assert(net_extract_cookie(tmp, "SAPISID", val, sizeof val) == 0 && !strcmp(val, "sap_net"));
    assert(net_extract_cookie(tmp, "SID", val, sizeof val) == 0 && !strcmp(val, "sid_val"));
    assert(net_commit_jar(tmp) == 0);
    assert(net_has_cookies() == 1);

    struct stat st;
    assert(stat(net_cookie_path(), &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(net_snapshot_jar(snap, sizeof snap) == 0);
    assert(strcmp(snap, net_cookie_path()) != 0);
    assert(net_extract_cookie(snap, "SAPISID", val, sizeof val) == 0 && !strcmp(val, "sap_net"));
    unlink(snap);

    yt_session s = {"UC123", "My Channel", "109876", "1", "AIzaKey", "1.2026"};
    assert(net_session_set(&s) == 0);
    assert(stat(net_session_path(), &st) == 0 && (st.st_mode & 0777) == 0600);
    yt_session loaded;
    assert(net_session_get(&loaded) == 0);
    assert(!strcmp(loaded.channel_id, "UC123"));
    assert(!strcmp(loaded.channel_name, "My Channel"));
    assert(!strcmp(loaded.delegated_session_id, "109876"));
    assert(!strcmp(loaded.session_index, "1"));
    assert(!strcmp(loaded.api_key, "AIzaKey"));

    net_auth_clear();
    assert(net_has_cookies() == 0);
    printf("ok   cookies, jar isolation & session\n");
}

int main(void) {
    test_sha1();
    test_json();
    test_form_and_util();
    test_paths();
    test_cookies_and_session();
    printf("All unit tests passed!\n");
    return 0;
}
