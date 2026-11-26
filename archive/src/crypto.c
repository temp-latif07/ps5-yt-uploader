#include "crypto.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- RFC 3174 SHA-1 Implementation --- */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} sha1_ctx_t;

static void sha1_init(sha1_ctx_t* ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        uint32_t val = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (val << 1) | (val >> 31);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }

        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d;
        d = c;
        c = (b << 30) | (b >> 2);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_update(sha1_ctx_t* ctx, const uint8_t* data, size_t len) {
    size_t buffer_len = (size_t)(ctx->count % 64);
    ctx->count += len;
    size_t i = 0;

    if (buffer_len > 0) {
        size_t needed = 64 - buffer_len;
        if (len < needed) {
            memcpy(&ctx->buffer[buffer_len], &data[0], len);
            return;
        }
        memcpy(&ctx->buffer[buffer_len], &data[0], needed);
        sha1_transform(ctx->state, ctx->buffer);
        i += needed;
    }

    while (i + 64 <= len) {
        sha1_transform(ctx->state, &data[i]);
        i += 64;
    }

    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

static void sha1_final(sha1_ctx_t* ctx, uint8_t digest[20]) {
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_len = (size_t)(ctx->count % 64);
    int i;

    ctx->buffer[buffer_len++] = 0x80;

    if (buffer_len > 56) {
        memset(&ctx->buffer[buffer_len], 0, 64 - buffer_len);
        sha1_transform(ctx->state, ctx->buffer);
        memset(&ctx->buffer[0], 0, 56);
    } else {
        memset(&ctx->buffer[buffer_len], 0, 56 - buffer_len);
    }

    for (i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)((total_bits >> ((7 - i) * 8)) & 0xFFU);
    }
    sha1_transform(ctx->state, ctx->buffer);

    for (i = 0; i < 5; i++) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xFFU);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFU);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFFU);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFFU);
    }
}

void sha1_hex(const unsigned char* data, size_t len, char hex_out[41]) {
    if (!hex_out) return;
    static const char hex_digits[] = "0123456789abcdef";
    uint8_t digest[20];
    sha1_ctx_t ctx;

    sha1_init(&ctx);
    if (data && len > 0) {
        sha1_update(&ctx, data, len);
    }
    sha1_final(&ctx, digest);

    for (int i = 0; i < 20; i++) {
        hex_out[i * 2]     = hex_digits[(digest[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_digits[digest[i] & 0x0F];
    }
    hex_out[40] = '\0';
}

/* --- SAPISIDHASH Generation --- */

int generate_sapisid_hash(const char* sapisid, const char* origin, long timestamp, char* hash_out, size_t max_len) {
    if (!sapisid || !origin || !hash_out || max_len == 0) {
        return -1;
    }

    int needed = snprintf(NULL, 0, "%ld %s %s", timestamp, sapisid, origin);
    if (needed < 0) {
        return -1;
    }

    char stack_buf[512];
    char* msg_buf = stack_buf;
    if ((size_t)needed + 1 > sizeof(stack_buf)) {
        msg_buf = (char*)malloc((size_t)needed + 1);
        if (!msg_buf) {
            return -1;
        }
    }

    snprintf(msg_buf, (size_t)needed + 1, "%ld %s %s", timestamp, sapisid, origin);

    char sha1_buf[41];
    sha1_hex((const unsigned char*)msg_buf, (size_t)needed, sha1_buf);

    if (msg_buf != stack_buf) {
        free(msg_buf);
    }

    int out_len = snprintf(NULL, 0, "%ld_%s", timestamp, sha1_buf);
    if (out_len < 0 || (size_t)out_len >= max_len) {
        return -1;
    }

    snprintf(hash_out, max_len, "%ld_%s", timestamp, sha1_buf);
    return 0;
}

/* --- Cookie Header Parser --- */

int extract_cookie_value(const char* cookie_header, const char* cookie_name, char* value_out, size_t max_len) {
    if (!cookie_header || !cookie_name || !value_out || max_len == 0) {
        return -1;
    }

    size_t target_name_len = strlen(cookie_name);
    if (target_name_len == 0) {
        return -1;
    }

    const char* p = cookie_header;

    while (*p != '\0') {
        // Skip leading whitespace or delimiters
        while (*p == ' ' || *p == '\t' || *p == ';') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        // Find the start and end of cookie name
        const char* name_start = p;
        while (*p != '\0' && *p != '=' && *p != ';' && *p != ' ' && *p != '\t') {
            p++;
        }
        const char* name_end = p;
        size_t name_len = (size_t)(name_end - name_start);

        // Skip any whitespace between cookie name and '='
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        if (*p != '=') {
            // Malformed pair without '=', skip to next delimiter
            while (*p != '\0' && *p != ';') {
                p++;
            }
            if (*p == ';') {
                p++;
            }
            continue;
        }

        // Skip '='
        p++;

        // Skip leading whitespace after '='
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        // Start of value
        const char* val_start = p;

        // Find end of cookie value (up to ';' or end of string)
        while (*p != '\0' && *p != ';') {
            p++;
        }
        const char* val_end = p;

        // Trim trailing whitespace from value
        while (val_end > val_start && (*(val_end - 1) == ' ' || *(val_end - 1) == '\t')) {
            val_end--;
        }

        // Check if cookie name matches target
        if (name_len == target_name_len && strncmp(name_start, cookie_name, name_len) == 0) {
            size_t val_len = (size_t)(val_end - val_start);

            // Strip surrounding double quotes if present
            if (val_len >= 2 && *val_start == '"' && *(val_end - 1) == '"') {
                val_start++;
                val_end--;
                val_len -= 2;
            }

            if (val_len >= max_len) {
                return -1; // buffer too small
            }

            if (val_len > 0) {
                memcpy(value_out, val_start, val_len);
            }
            value_out[val_len] = '\0';
            return 0;
        }

        if (*p == ';') {
            p++;
        }
    }

    return -1; // Not found
}
