// Minimal JSON reading for API/config responses.
#include "app.h"

#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *skip_string(const char *p) {
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *skip_value(const char *p) {
    p = ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

const char *json_find(const char *obj, const char *key) {
    if (!obj || !key) return NULL;
    const char *p = ws(obj);
    if (*p != '{') return NULL;
    p++;
    size_t kl = strlen(key);
    for (;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *ks = p + 1, *ke = skip_string(p);
        if (!ke) return NULL;
        p = ws(ke);
        if (*p != ':') return NULL;
        const char *val = ws(p + 1);
        if ((size_t)(ke - 1 - ks) == kl && !strncmp(ks, key, kl)) return val;
        p = skip_value(val);
        if (!p) return NULL;
        p = ws(p);
        if (*p == ',') p++;
        else return NULL;
    }
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

int json_string(const char *val, char *out, size_t len) {
    if (!val || *val != '"' || !len) return -1;
    size_t o = 0;
    for (const char *p = val + 1; *p; p++) {
        if (*p == '"') {
            out[o] = 0;
            return 0;
        }
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                int u = hex4(p + 1);
                if (u < 0) return -1;
                p += 4;
                char tmp[4];
                int n = 0;
                if (u < 0x80) tmp[n++] = (char)u;
                else if (u < 0x800) tmp[n++] = (char)(0xc0 | (u >> 6)), tmp[n++] = (char)(0x80 | (u & 63));
                else tmp[n++] = (char)(0xe0 | (u >> 12)), tmp[n++] = (char)(0x80 | ((u >> 6) & 63)),
                     tmp[n++] = (char)(0x80 | (u & 63));
                for (int i = 0; i < n && o + 1 < len; i++) out[o++] = tmp[i];
                continue;
            }
            case 0: return -1;
            default: c = (unsigned char)*p;
            }
        }
        if (o + 1 < len) out[o++] = (char)c;
    }
    return -1;
}

int json_bool(const char *val) {
    if (!val) return -1;
    if (!strncmp(val, "true", 4)) return 1;
    if (!strncmp(val, "false", 5)) return 0;
    return -1;
}

long long json_int(const char *val, long long def) {
    if (!val) return def;
    char *end;
    long long v = strtoll(val, &end, 10);
    return end == val ? def : v;
}

const char *json_array_first(const char *arr) {
    if (!arr) return NULL;
    const char *p = ws(arr);
    if (*p != '[') return NULL;
    p = ws(p + 1);
    return *p == ']' || !*p ? NULL : p;
}

const char *json_array_next(const char *elem) {
    const char *p = skip_value(elem);
    if (!p) return NULL;
    p = ws(p);
    if (*p != ',') return NULL;
    p = ws(p + 1);
    return *p ? p : NULL;
}
