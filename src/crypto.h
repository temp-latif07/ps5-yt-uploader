#ifndef PS5_YOUTUBE_CRYPTO_H
#define PS5_YOUTUBE_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Computes the SHA-1 hash of the given data and formats it as a lowercase
 * 40-character hexadecimal string with a null terminator (41 bytes total).
 *
 * @param data     Pointer to input bytes.
 * @param len      Length of data in bytes.
 * @param hex_out  Destination buffer of at least 41 bytes.
 */
void sha1_hex(const unsigned char* data, size_t len, char hex_out[41]);

/**
 * Generates Google's SAPISIDHASH Authorization token:
 *   Format: "{timestamp}_{sha1("{timestamp} {sapisid} {origin}")}"
 *
 * @param sapisid    The SAPISID or __Secure-3PAPISID cookie value.
 * @param origin     Origin URL, typically "https://studio.youtube.com".
 * @param timestamp  Unix epoch timestamp in seconds.
 * @param hash_out   Destination buffer.
 * @param max_len    Capacity of hash_out in bytes. Must be >= 52 for standard 10-digit timestamp.
 * @return 0 on success, -1 on failure (e.g. invalid arguments or buffer too small).
 */
int generate_sapisid_hash(const char* sapisid, const char* origin, long timestamp, char* hash_out, size_t max_len);

/**
 * Extracts the value of a specific cookie from a standard HTTP Cookie header string.
 * Handles whitespace around delimiters and strips surrounding double quotes if present.
 *
 * @param cookie_header Pointer to the raw Cookie header string (e.g., "A=1; B=2").
 * @param cookie_name   Name of the cookie to extract (e.g., "SAPISID").
 * @param value_out     Destination buffer for the extracted cookie value.
 * @param max_len       Capacity of value_out including null terminator.
 * @return 0 on success, -1 on failure (e.g. cookie not found, buffer too small, null args).
 */
int extract_cookie_value(const char* cookie_header, const char* cookie_name, char* value_out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* PS5_YOUTUBE_CRYPTO_H */
