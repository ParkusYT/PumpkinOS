/* ===========================================================================
 * PumpkinOS - minimal freestanding string / memory helpers
 * ---------------------------------------------------------------------------
 * GCC is free to emit calls to memset/memcpy/memmove/memcmp even with
 * -ffreestanding (e.g. to initialise structs or arrays), so we must provide
 * them ourselves. The str* helpers are used by the shell's command parser.
 * ========================================================================= */
#ifndef PUMPKIN_STRING_H
#define PUMPKIN_STRING_H

#include <stddef.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

#endif /* PUMPKIN_STRING_H */
