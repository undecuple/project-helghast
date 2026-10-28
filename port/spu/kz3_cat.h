/* kz3_cat: append a formatted string to a fixed buffer for the diagnostic log lines.
 * Appends to b (size n) at k and returns the new end, never past n - 1. snprintf returns the length it wanted, so
 * a plain k += snprintf(b + k, n - k, ...) can run k past the buffer and turn n - k into a huge size. */
#ifndef KZ3_CAT_H
#define KZ3_CAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static inline int kz3_cat(char* b, size_t n, int k, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static inline int kz3_cat(char* b, size_t n, int k, const char* fmt, ...)
{
    if (!n) return 0;
    if (k < 0 || (size_t)k >= n) return (int)n - 1;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(b + k, n - (size_t)k, fmt, ap);
    va_end(ap);
    if (w < 0) return k;
    return (size_t)k + (size_t)w >= n ? (int)n - 1 : k + w;
}

#endif
