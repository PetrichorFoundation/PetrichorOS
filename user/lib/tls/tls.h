/* TLS 1.2 client: ECDHE key exchange with AES-GCM, servers verified against /etc/ssl/roots.bin. */
#pragma once

#define TLS_INSECURE 1                  /* skip certificate validation (the signature is still checked) */

struct tls;

/* Handshakes over the connected socket `fd`. On failure returns NULL and sets *err to a readable reason. */
struct tls *tls_connect(int fd, const char *host, int flags, const char **err);
int tls_read(struct tls *t, void *buf, int n);          /* > 0 bytes, 0 at the end of the stream, < 0 on error */
int tls_write(struct tls *t, const void *buf, int n);   /* writes everything, or returns < 0 */
void tls_close(struct tls *t);                          /* sends close_notify and frees t (the socket stays open) */
const char *tls_error(struct tls *t);
