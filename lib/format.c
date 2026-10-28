#include "format.h"
#include "string.h"

/* Writes digits least-significant first; returns the digit count. */
static int utoa(char *buf, u32 v, u32 base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);
    return n;
}

static void fill(fmt_out out, void *ctx, char c, int n, int *count)
{
    for (; n > 0; n--) {
        out(c, ctx);
        (*count)++;
    }
}

int vformat(fmt_out out, void *ctx, const char *fmt, va_list ap)
{
    int count = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(*fmt, ctx);
            count++;
            continue;
        }
        fmt++;

        int left = 0, zero = 0, width = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z')
            fmt++;

        char tmp[16];
        const char *s = tmp;
        int len = 0, numeric = 0;
        char sign = 0;

        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            u32 u = v;
            if (v < 0) {
                sign = '-';
                u = -u;
            }
            len = utoa(tmp, u, 10, 0);
            numeric = 1;
            break;
        }
        case 'u':
            len = utoa(tmp, va_arg(ap, u32), 10, 0);
            numeric = 1;
            break;
        case 'x':
            len = utoa(tmp, va_arg(ap, u32), 16, 0);
            numeric = 1;
            break;
        case 'X':
            len = utoa(tmp, va_arg(ap, u32), 16, 1);
            numeric = 1;
            break;
        case 'p':
            out('0', ctx);
            out('x', ctx);
            count += 2;
            len = utoa(tmp, (u32)va_arg(ap, void *), 16, 0);
            numeric = 1;
            zero = 1;
            left = 0;
            width = 8;
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            len = strlen(s);
            break;
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        case 0:
            return count;
        default:
            tmp[0] = '%';
            tmp[1] = *fmt;
            len = 2;
            break;
        }

        int total = len + (sign ? 1 : 0);
        if (!left && !(numeric && zero))
            fill(out, ctx, ' ', width - total, &count);
        if (sign) {
            out(sign, ctx);
            count++;
        }
        if (!left && numeric && zero)
            fill(out, ctx, '0', width - total, &count);
        if (numeric) {
            for (int i = len - 1; i >= 0; i--)
                out(tmp[i], ctx);
        } else {
            for (int i = 0; i < len; i++)
                out(s[i], ctx);
        }
        count += len;
        if (left)
            fill(out, ctx, ' ', width - total, &count);
    }
    return count;
}
