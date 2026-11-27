// Unit tests for core utilities, JSON, path checks, and OAuth credential/token cache management.
#include "app.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const unsigned char cacert_pem[] = "";
const size_t cacert_pem_len = 0;
const unsigned char tile_icon_png[] = "";
const size_t tile_icon_png_len = 0;
void api_handle(http_req *req) { (void)req; }

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
    assert(is_video_filename("oauth.txt") == 0);
    assert(is_video_filename("/data/ps5-yt-uploader/oauth.txt") == 0);
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

static void test_oauth_and_tokens(void) {
    setenv("PS5YT_APP_DIR", "/tmp/ps5yt_unit_net", 1);
    plat_init();
    net_init();
    net_oauth_clear_all();

    char cid[256], csec[256], err[256];
    const char *installed_json =
        "\xEF\xBB\xBF{\"installed\":{\"client_id\":\" 12345-abc.apps.googleusercontent.com \","
        "\"project_id\":\"demo\",\"client_secret\":\" GOCSPX-secret-val \"}}";
    assert(net_parse_client_json(installed_json, cid, sizeof cid, csec, sizeof csec, err, sizeof err) == 0);
    assert(!strcmp(cid, "12345-abc.apps.googleusercontent.com"));
    assert(!strcmp(csec, "GOCSPX-secret-val"));

    const char *web_json = "{\"web\":{\"client_id\":\"web-id\",\"client_secret\":\"web-sec\"}}";
    assert(net_parse_client_json(web_json, cid, sizeof cid, csec, sizeof csec, err, sizeof err) == 0);
    assert(!strcmp(cid, "web-id") && !strcmp(csec, "web-sec"));

    const char *flat_json = "{\"client_id\":\"flat-id\",\"client_secret\":\"flat-sec\"}";
    assert(net_parse_client_json(flat_json, cid, sizeof cid, csec, sizeof csec, err, sizeof err) == 0);
    assert(!strcmp(cid, "flat-id") && !strcmp(csec, "flat-sec"));

    assert(net_parse_client_json("{\"installed\":{\"client_id\":\"only-id\"}}",
                                 cid, sizeof cid, csec, sizeof csec, err, sizeof err) == -1);
    assert(strstr(err, "client_secret") != NULL);
    assert(net_parse_client_json("not json", cid, sizeof cid, csec, sizeof csec, err, sizeof err) == -1);

    assert(net_oauth_has_client() == 0);
    assert(net_oauth_is_logged_in() == 0);

    yt_oauth o = {
        "12345-abc.apps.googleusercontent.com",
        "GOCSPX-secret-val",
        "1//refresh-token",
        "UC_CHANNEL_1",
        "Test Gamer"
    };
    assert(net_oauth_set(&o) == 0);
    struct stat st;
    assert(stat(net_oauth_path(), &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(net_oauth_has_client() == 1);
    assert(net_oauth_is_logged_in() == 1);

    yt_oauth loaded;
    assert(net_oauth_get(&loaded) == 0);
    assert(!strcmp(loaded.client_id, o.client_id));
    assert(!strcmp(loaded.client_secret, o.client_secret));
    assert(!strcmp(loaded.refresh_token, o.refresh_token));
    assert(!strcmp(loaded.channel_id, o.channel_id));
    assert(!strcmp(loaded.channel_name, o.channel_name));

    char tok[128];
    net_token_cache_set("short-lived", 30);
    assert(net_token_cache_get(tok, sizeof tok) == -1);
    net_token_cache_set("valid-access-tok", 3600);
    assert(net_token_cache_get(tok, sizeof tok) == 0 && !strcmp(tok, "valid-access-tok"));

    net_oauth_clear_tokens();
    assert(net_oauth_has_client() == 1);
    assert(net_oauth_is_logged_in() == 0);
    assert(net_token_cache_get(tok, sizeof tok) == -1);
    assert(net_oauth_get(&loaded) == 0);
    assert(!strcmp(loaded.client_id, o.client_id));
    assert(!strcmp(loaded.client_secret, o.client_secret));
    assert(loaded.refresh_token[0] == 0 && loaded.channel_id[0] == 0);

    net_oauth_clear_all();
    assert(net_oauth_has_client() == 0);
    assert(net_oauth_get(&loaded) == -1);
    printf("ok   OAuth JSON parsing, oauth.txt mode 600 & token cache\n");
}

int main(void) {
    test_json();
    test_form_and_util();
    test_paths();
    test_oauth_and_tokens();
    printf("All unit tests passed!\n");
    return 0;
}
