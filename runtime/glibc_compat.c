/*
 * Linux portable build (PSIV_PORTABLE, x86-64): the program runs on glibc
 * 2.35 (Ubuntu 22.04, Debian 12, the Steam Deck) though it's built against a
 * newer one. Newer glibcs gave some functions new symbol versions (the old
 * ones are still there) or added them outright. Each is defined here once,
 * so every reference in the program -- ours, SDL's, libstdc++'s -- binds to
 * this file, and these call the old versions or do the job themselves.
 *
 * Check a build with:  objdump -T psiv | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
 */
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <sys/random.h>

/* the old symbol under a name of our own */
#define OLD(ret, name, params, ver)                                     \
    ret psiv_old_##name params;                                         \
    __asm__(".symver psiv_old_" #name "," #name "@" ver)

/* math: new versions in 2.38 (fmod, fmodf) and 2.43 (the rest) */
OLD(float, acosf, (float), "GLIBC_2.2.5");
OLD(float, asinf, (float), "GLIBC_2.2.5");
OLD(float, atan2f, (float, float), "GLIBC_2.2.5");
OLD(float, log10f, (float), "GLIBC_2.2.5");
OLD(float, sqrtf, (float), "GLIBC_2.2.5");
OLD(double, fmod, (double, double), "GLIBC_2.2.5");
OLD(float, fmodf, (float, float), "GLIBC_2.2.5");

float acosf(float x) { return psiv_old_acosf(x); }
float asinf(float x) { return psiv_old_asinf(x); }
float atan2f(float y, float x) { return psiv_old_atan2f(y, x); }
float log10f(float x) { return psiv_old_log10f(x); }
float sqrtf(float x) { return psiv_old_sqrtf(x); }
double fmod(double x, double y) { return psiv_old_fmod(x, y); }
float fmodf(float x, float y) { return psiv_old_fmodf(x, y); }

/* C23's number parsing (2.38): headers send code built with _GNU_SOURCE
 * (SDL, C++) here; the only difference, 0b binary literals, isn't used */
long __isoc23_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
long long __isoc23_strtoll(const char *s, char **end, int base) { return strtoll(s, end, base); }
unsigned long __isoc23_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }
unsigned long long __isoc23_strtoull(const char *s, char **end, int base) { return strtoull(s, end, base); }
long __isoc23_wcstol(const wchar_t *s, wchar_t **end, int base) { return wcstol(s, end, base); }
int __isoc23_vsscanf(const char *s, const char *fmt, va_list ap) { return vsscanf(s, fmt, ap); }

int __isoc23_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}

int __isoc23_fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

/* new in 2.38 */
size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t n) {
    size_t d = strnlen(dst, n);
    if (d == n) return n + strlen(src);
    return d + strlcpy(dst + d, src, n - d);
}

size_t wcslcpy(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t len = wcslen(src);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        wmemcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}

size_t wcslcat(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t d = wcsnlen(dst, n);
    if (d == n) return n + wcslen(src);
    return d + wcslcpy(dst + d, src, n - d);
}

/* new in 2.36 (std::random_device): getrandom is 2.25 */
uint32_t arc4random(void) {
    uint32_t v = 0;
    if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) v = (uint32_t)time(NULL) * 2654435761u;
    return v;
}

#endif
