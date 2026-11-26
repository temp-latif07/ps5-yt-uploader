// RFC 3174 SHA-1 and YouTube Studio SAPISIDHASH computation.
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} sha1_ctx;

static void sha1_init(sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof ctx->buffer);
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        uint32_t val = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (val << 1) | (val >> 31);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
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

static void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len) {
    size_t buffer_len = (size_t)(ctx->count % 64);
    ctx->count += len;
    size_t i = 0;
    if (buffer_len > 0) {
        size_t needed = 64 - buffer_len;
        if (len < needed) {
            memcpy(&ctx->buffer[buffer_len], data, len);
            return;
        }
        memcpy(&ctx->buffer[buffer_len], data, needed);
        sha1_transform(ctx->state, ctx->buffer);
        i += needed;
    }
    while (i + 64 <= len) {
        sha1_transform(ctx->state, &data[i]);
        i += 64;
    }
    if (i < len) memcpy(ctx->buffer, &data[i], len - i);
}

static void sha1_final(sha1_ctx *ctx, uint8_t digest[20]) {
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_len = (size_t)(ctx->count % 64);
    ctx->buffer[buffer_len++] = 0x80;
    if (buffer_len > 56) {
        memset(&ctx->buffer[buffer_len], 0, 64 - buffer_len);
        sha1_transform(ctx->state, ctx->buffer);
        memset(ctx->buffer, 0, 56);
    } else {
        memset(&ctx->buffer[buffer_len], 0, 56 - buffer_len);
    }
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)((total_bits >> ((7 - i) * 8)) & 0xFFU);
    sha1_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xFFU);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFU);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFFU);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFFU);
    }
}

void sha1_hex(const unsigned char *data, size_t len, char hex_out[41]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[20];
    sha1_ctx ctx;
    sha1_init(&ctx);
    if (data && len > 0) sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
    for (int i = 0; i < 20; i++) {
        hex_out[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    hex_out[40] = 0;
}

int sapisid_hash(const char *sapisid, const char *origin, long ts, char *out, size_t len) {
    if (!sapisid || !sapisid[0] || !origin || !out || len < 48) return -1;
    int needed = snprintf(NULL, 0, "%ld %s %s", ts, sapisid, origin);
    if (needed < 0) return -1;
    char stack[512];
    char *msg = (size_t)needed + 1 <= sizeof stack ? stack : malloc((size_t)needed + 1);
    if (!msg) return -1;
    snprintf(msg, (size_t)needed + 1, "%ld %s %s", ts, sapisid, origin);
    char hex[41];
    sha1_hex((const unsigned char *)msg, (size_t)needed, hex);
    memset(msg, 0, (size_t)needed);
    if (msg != stack) free(msg);
    int n = snprintf(out, len, "%ld_%s", ts, hex);
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}
