#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/crypto.h"

void test_sha1(void) {
    char out[41];

    // RFC 3174 test vectors
    // 1. "test"
    sha1_hex((const unsigned char*)"test", 4, out);
    assert(strcmp(out, "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3") == 0);

    // 2. "" (empty string)
    sha1_hex((const unsigned char*)"", 0, out);
    assert(strcmp(out, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);

    // 3. "abc"
    sha1_hex((const unsigned char*)"abc", 3, out);
    assert(strcmp(out, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);

    // 4. Multi-block string (56 bytes, crosses 55 byte boundary for 64-byte block padding)
    const char* str4 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1_hex((const unsigned char*)str4, strlen(str4), out);
    assert(strcmp(out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);
}

void test_sapisid_hash_generation(void) {
    const char* sapisid = "SampleSapisid123";
    const char* origin = "https://studio.youtube.com";
    long timestamp = 1700000000;
    char hash_buf[128];

    // "{timestamp} {sapisid} {origin}" -> "1700000000 SampleSapisid123 https://studio.youtube.com"
    // sha1("1700000000 SampleSapisid123 https://studio.youtube.com") = 9bb175df8811b39190edc85b384b0665aba1b83e
    assert(generate_sapisid_hash(sapisid, origin, timestamp, hash_buf, sizeof(hash_buf)) == 0);
    assert(strncmp(hash_buf, "1700000000_", 11) == 0);
    assert(strcmp(hash_buf, "1700000000_9bb175df8811b39190edc85b384b0665aba1b83e") == 0);

    // Check with default/standard origin
    assert(generate_sapisid_hash(sapisid, "https://www.youtube.com", 1609459200, hash_buf, sizeof(hash_buf)) == 0);
    assert(strncmp(hash_buf, "1609459200_", 11) == 0);
    assert(strlen(hash_buf) == 11 + 40);

    // Buffer too small checks:
    // "1700000000_9bb175df8811b39190edc85b384b0665aba1b83e" is 51 characters + 1 null terminator = 52 bytes
    char small_buf[51];
    assert(generate_sapisid_hash(sapisid, origin, timestamp, small_buf, sizeof(small_buf)) == -1);

    char exact_buf[52];
    assert(generate_sapisid_hash(sapisid, origin, timestamp, exact_buf, sizeof(exact_buf)) == 0);
    assert(strcmp(exact_buf, "1700000000_9bb175df8811b39190edc85b384b0665aba1b83e") == 0);

    // Null and edge arguments
    assert(generate_sapisid_hash(NULL, origin, timestamp, hash_buf, sizeof(hash_buf)) == -1);
    assert(generate_sapisid_hash(sapisid, NULL, timestamp, hash_buf, sizeof(hash_buf)) == -1);
    assert(generate_sapisid_hash(sapisid, origin, timestamp, NULL, sizeof(hash_buf)) == -1);
    assert(generate_sapisid_hash(sapisid, origin, timestamp, hash_buf, 0) == -1);
}

void test_cookie_extraction(void) {
    const char* cookie_str = "VISITOR_INFO1_LIVE=abc; SAPISID=RealSapisidValue999; LOGIN_INFO=xyz";
    char val[128];

    // Standard extraction
    assert(extract_cookie_value(cookie_str, "SAPISID", val, sizeof(val)) == 0);
    assert(strcmp(val, "RealSapisidValue999") == 0);

    // First cookie
    assert(extract_cookie_value(cookie_str, "VISITOR_INFO1_LIVE", val, sizeof(val)) == 0);
    assert(strcmp(val, "abc") == 0);

    // Last cookie
    assert(extract_cookie_value(cookie_str, "LOGIN_INFO", val, sizeof(val)) == 0);
    assert(strcmp(val, "xyz") == 0);

    // Non-existent cookie
    assert(extract_cookie_value(cookie_str, "NONEXISTENT", val, sizeof(val)) == -1);

    // Substring protection: "SID" should not match "SAPISID"
    assert(extract_cookie_value(cookie_str, "SID", val, sizeof(val)) == -1);
    assert(extract_cookie_value(cookie_str, "APISID", val, sizeof(val)) == -1);
    assert(extract_cookie_value(cookie_str, "INFO", val, sizeof(val)) == -1);

    // Whitespace handling around semicolons and '='
    const char* ws_cookie = "  PREF = f1=50000000 ;   SAPISID  =   MySecret123  ; foo=bar  ";
    assert(extract_cookie_value(ws_cookie, "SAPISID", val, sizeof(val)) == 0);
    assert(strcmp(val, "MySecret123") == 0);
    assert(extract_cookie_value(ws_cookie, "PREF", val, sizeof(val)) == 0);
    assert(strcmp(val, "f1=50000000") == 0);
    assert(extract_cookie_value(ws_cookie, "foo", val, sizeof(val)) == 0);
    assert(strcmp(val, "bar") == 0);

    // Quoted value handling
    const char* quoted_cookie = "SAPISID=\"quoted_sapisid_token\"; other=value";
    assert(extract_cookie_value(quoted_cookie, "SAPISID", val, sizeof(val)) == 0);
    assert(strcmp(val, "quoted_sapisid_token") == 0);

    // Empty cookie value
    const char* empty_val_cookie = "EMPTY=; NOT_EMPTY=present";
    assert(extract_cookie_value(empty_val_cookie, "EMPTY", val, sizeof(val)) == 0);
    assert(strcmp(val, "") == 0);
    assert(extract_cookie_value(empty_val_cookie, "NOT_EMPTY", val, sizeof(val)) == 0);
    assert(strcmp(val, "present") == 0);

    // Buffer too small check
    char tiny[5]; // Can fit at most 4 chars + '\0'
    assert(extract_cookie_value(cookie_str, "SAPISID", tiny, sizeof(tiny)) == -1);
    char just_fits[4]; // "abc" is 3 chars + '\0' = 4 bytes
    assert(extract_cookie_value(cookie_str, "VISITOR_INFO1_LIVE", just_fits, sizeof(just_fits)) == 0);
    assert(strcmp(just_fits, "abc") == 0);

    // Invalid arguments
    assert(extract_cookie_value(NULL, "SAPISID", val, sizeof(val)) == -1);
    assert(extract_cookie_value(cookie_str, NULL, val, sizeof(val)) == -1);
    assert(extract_cookie_value(cookie_str, "SAPISID", NULL, sizeof(val)) == -1);
    assert(extract_cookie_value(cookie_str, "SAPISID", val, 0) == -1);
}

int main(void) {
    test_sha1();
    test_sapisid_hash_generation();
    test_cookie_extraction();
    printf("Task 2 Crypto Tests Passed Successfully!\n");
    return 0;
}
