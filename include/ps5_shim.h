#ifndef PS5_SHIM_H
#define PS5_SHIM_H

#if defined(PS5)

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Disable fortify source wrappers (__memcpy_chk, __sprintf_chk) */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
#define _FORTIFY_SOURCE 0

/* Neutralize Apple Darwin symbol aliasing and inode64 suffixes when cross-compiling on macOS */
#ifdef __asm
#undef __asm
#endif
#define __asm(x)

#define __DARWIN_ONLY_64_BIT_INO_T 1
#define __DARWIN_ONLY_UNIX_CONFORMANCE 1
#define __DARWIN_ONLY_VERS_1050 1
#define __DARWIN_UNIX03 0

#define __DARWIN_ALIAS(sym)
#define __DARWIN_ALIAS_C(sym)
#define __DARWIN_ALIAS_I(sym)
#define __DARWIN_INODE64(sym)
#define __DARWIN_1050INODE64(sym)
#define __DARWIN_1050ALIAS(sym)
#define __DARWIN_1050ALIAS_C(sym)
#define __DARWIN_1050ALIAS_I(sym)
#define __DARWIN_1050(sym)
#define __DARWIN_NOCANCEL(sym)
#define __DARWIN_EXTSN(sym)
#define __DARWIN_EXTSN_C(sym)
#define __DARWIN_ALIAS_STARTING(_mac, _iphone, x)

/*
 * Neutralize Darwin's _ctype.h which pulls in macOS-only _DefaultRuneLocale
 * and __maskrune symbols. Provide clean-room C99 ASCII implementations.
 */
#ifndef _CTYPE_H_
#define _CTYPE_H_
#define __CTYPE_H_

static inline int isalnum(int c) {
    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}
static inline int isalpha(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}
static inline int iscntrl(int c) {
    return ((c >= 0 && c <= 31) || c == 127);
}
static inline int isdigit(int c) {
    return (c >= '0' && c <= '9');
}
static inline int isgraph(int c) {
    return (c >= 33 && c <= 126);
}
static inline int islower(int c) {
    return (c >= 'a' && c <= 'z');
}
static inline int isprint(int c) {
    return (c >= 32 && c <= 126);
}
static inline int ispunct(int c) {
    return (isgraph(c) && !isalnum(c));
}
static inline int isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}
static inline int isupper(int c) {
    return (c >= 'A' && c <= 'Z');
}
static inline int isxdigit(int c) {
    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}
static inline int tolower(int c) {
    return ((c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c);
}
static inline int toupper(int c) {
    return ((c >= 'a' && c <= 'z') ? (c - ('a' - 'A')) : c);
}
static inline int isascii(int c) {
    return ((c & ~0x7F) == 0);
}
static inline int toascii(int c) {
    return (c & 0x7F);
}

#endif /* _CTYPE_H_ */

#endif /* PS5 */

#endif /* PS5_SHIM_H */
