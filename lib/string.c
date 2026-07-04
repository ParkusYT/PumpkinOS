/* ===========================================================================
 * PumpkinOS - minimal freestanding string / memory helpers
 * ========================================================================= */
#include "string.h"

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n--) {
        if (*a != *b)
            return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (*a == '\0')
            return 0;
        a++;
        b++;
    }
    return 0;
}
