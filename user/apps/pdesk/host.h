/* Hosting the windows of separate programs (host.c). */
#pragma once
#include "winproto.h"

#define HOST_WINS 8

struct hwin;
struct host {
    int up, down, pid;                          /* pid 0: unknown (programs started from a terminal) */
    struct hwin *w[HOST_WINS];
};

int host_msg(struct host *h, const struct pw_msg *m, const u8 *payload);   /* 1 if it was a shared-memory window message */
void host_close_all(struct host *h);
int app_launch(const char *path, const char *arg);                          /* start a program the desktop hosts */
int hosts_pollfds(struct pollfd *p, int max);
void hosts_fdready(struct pollfd *p);
int browser_installed(void);
