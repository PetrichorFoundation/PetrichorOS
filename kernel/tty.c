/* Line-discipline for the console: echo, editing, ^C, ^D. */
#include "kernel.h"

#define TTY_BUF 1024

static char ready[TTY_BUF];
static u32 rhead, rtail;
static char line[256];
static u32 llen;
static int eof_flag;

int tty_fg_pgrp;

static void ring_put(char c)
{
    if (rtail - rhead < TTY_BUF)
        ready[rtail++ % TTY_BUF] = c;
}

static void flush_line(void)
{
    for (u32 i = 0; i < llen; i++)
        ring_put(line[i]);
    llen = 0;
}

static void erase_one(void)
{
    llen--;
    console_write("\b \b", 3);
}

void tty_input(char c)
{
    if (c == '\r')
        c = '\n';

    if (c == 3) {                               /* ^C */
        console_write("^C\n", 3);
        llen = 0;
        if (!proc_kill_pgrp(tty_fg_pgrp))
            ring_put('\n');                     /* nothing running: wake the shell */
    } else if (c == 4) {                        /* ^D */
        if (llen == 0)
            eof_flag = 1;
        else
            flush_line();
    } else if (c == '\b' || c == 127) {
        if (llen)
            erase_one();
        return;
    } else if (c == 21) {                       /* ^U */
        while (llen)
            erase_one();
        return;
    } else if (c == '\n') {
        console_putc('\n');
        line[llen++] = '\n';
        flush_line();
    } else if (c >= ' ' || c == '\t') {
        if (llen < sizeof line - 2) {
            line[llen++] = c;
            console_putc(c);
        }
        return;
    } else {
        return;
    }
    wakeup(ready);
}

int tty_read(char *buf, u32 n)
{
    if (n == 0)
        return 0;
    for (;;) {
        if (rhead != rtail) {
            u32 i = 0;
            while (i < n && rhead != rtail) {
                char c = ready[rhead++ % TTY_BUF];
                buf[i++] = c;
                if (c == '\n')
                    break;
            }
            return i;
        }
        if (eof_flag) {
            eof_flag = 0;
            return 0;
        }
        if (cur->killed)
            return -EINTR;
        sleep_on(ready);
    }
}
