#pragma once
#include "types.h"

/* Minimal printf engine shared by the kernel and libc.
 * Supports %d %i %u %x %X %c %s %p %% with '-', '0', width and '*'. */
typedef void (*fmt_out)(char c, void *ctx);
int vformat(fmt_out out, void *ctx, const char *fmt, va_list ap);
