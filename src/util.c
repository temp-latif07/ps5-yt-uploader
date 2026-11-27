// Small helpers: growable string buffer, JSON string output, paths, form/URL encoding.
#include "app.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void sb_init(sbuf *b) { b->data = NULL, b->len = b->cap = 0; }
void sb_free(sbuf *b) { free(b->data), sb_init(b); }

int sb_add(sbuf *b, const void *data, size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + len + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p, b->cap = cap;
    }
    if (len > 0) memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 0;
}

int sb_puts(sbuf *b, const char *s) { return s ? sb_add(b, s, strlen(s)) : 0; }

int sb_printf(sbuf *b, const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof small) return sb_add(b, small, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = sb_add(b, big, (size_t)n);
    free(big);
    return rc;
}

void sb_json_str(sbuf *b, const char *s) {
    sb_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': sb_add(b, "\\\"", 2); break;
        case '\\': sb_add(b, "\\\\", 2); break;
        case '\n': sb_add(b, "\\n", 2); break;
        case '\r': sb_add(b, "\\r", 2); break;
        case '\t': sb_add(b, "\\t", 2); break;
        default:
            if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
            else sb_add(b, p, 1);
        }
    }
    sb_add(b, "\"", 1);
}

int mkdir_p(const char *path) {
    char tmp[1024];
    if (!path || !path[0] || strlen(path) >= sizeof tmp) return -1;
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
    return 0;
}

void sanitize_name(char *s) {
    if (!s) return;
    for (unsigned char *p = (unsigned char *)s; *p; p++)
        if (*p < 0x20 || strchr("/\\:*?\"<>|", *p)) *p = '_';
    size_t n = strlen(s);
    while (n && (s[n - 1] == '.' || s[n - 1] == ' ')) s[--n] = 0;
    size_t lead = 0;
    while (s[lead] == '.' || s[lead] == ' ') lead++;
    if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
    if (strlen(s) > 240) s[240] = 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') *o++ = ' ';
        else if (*p == '%' && hexval((unsigned char)p[1]) >= 0 && hexval((unsigned char)p[2]) >= 0) {
            *o++ = (char)(hexval((unsigned char)p[1]) * 16 + hexval((unsigned char)p[2]));
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
    return (int)(o - s);
}

const char *form_get(const char *form, const char *key, char *out, size_t len) {
    if (!out || !len) return NULL;
    size_t kl = strlen(key);
    const char *p = form;
    out[0] = 0;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t n = (size_t)(end - (p + kl + 1));
            if (n >= len) n = len - 1;
            memcpy(out, p + kl + 1, n);
            out[n] = 0;
            url_decode(out);
            return out;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

char *form_get_alloc(const char *form, const char *key, size_t max_len) {
    size_t kl = strlen(key);
    const char *p = form;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) >= kl + 1 && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t n = (size_t)(end - (p + kl + 1));
            if (n > max_len) n = max_len;
            char *out = malloc(n + 1);
            if (!out) return NULL;
            if (n > 0) memcpy(out, p + kl + 1, n);
            out[n] = 0;
            url_decode(out);
            return out;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

void url_encode(const char *in, char *out, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 4 < len; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
            out[o++] = (char)*p;
        else out[o++] = '%', out[o++] = hex[*p >> 4], out[o++] = hex[*p & 15];
    }
    if (len) out[o] = 0;
}

char *str_trim(char *s) {
    if (!s) return s;
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) n--;
    if (p != s && n) memmove(s, p, n);
    s[n] = 0;
    return s;
}

int is_video_filename(const char *name) {
    if (!name || !name[0]) return 0;
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    if (!base[0] || base[0] == '.') return 0;
    size_t len = strlen(base);
    if (len > 4 && !strcasecmp(base + len - 4, ".mp4")) return 1;
    if (len > 5 && !strcasecmp(base + len - 5, ".webm")) return 1;
    return 0;
}
