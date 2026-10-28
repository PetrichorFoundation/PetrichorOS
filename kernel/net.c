/* Network stack: Ethernet, ARP, IPv4, ICMP echo, UDP, and a TCP client.
 *
 * Receive runs from the NIC interrupt; transmit uses one static frame buffer.
 * That is safe because the kernel never runs with interrupts enabled, so the
 * two can't overlap.  Processes block on a socket (or &arp_chan / &ping_chan)
 * and are woken by the receive path or by net_tick() timeouts.
 *
 * IPv4 addresses are u32 in host order throughout. */
#include "kernel.h"

#define IP(a, b, c, d) (((u32)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_MSS   1400
#define RX_RING   8192
#define NSOCK     16
#define UDP_QLEN  4
#define RTO_TICKS 100
#define RTX_MAX   5

/* QEMU user-mode networking defaults. */
static struct {
    u32 ip, mask, gw, dns;
} cfg = {IP(10, 0, 2, 15), IP(255, 255, 255, 0), IP(10, 0, 2, 2), IP(10, 0, 2, 3)};

static int net_up;
static u32 rx_packets, tx_packets;

/* ---------------- byte order and checksums ---------------- */

static void put16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v; }
static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u16 get16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static u32 sum_bytes(const u8 *p, u32 len, u32 acc)
{
    for (; len > 1; len -= 2, p += 2)
        acc += p[0] << 8 | p[1];
    if (len)
        acc += p[0] << 8;
    return acc;
}

static u16 fold(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

/* ---------------- sockets ---------------- */

enum { TS_CLOSED, TS_SYN_SENT, TS_ESTAB, TS_FIN_WAIT, TS_CLOSE_WAIT, TS_DEAD };

struct dgram {
    u32 ip;
    u16 port, len;
    u8 data[1472];
};

struct sock {
    int type, state, err;
    u16 lport, rport;
    u32 rip;
    u32 timeout_ms;
    int closing;
    u32 close_deadline;

    struct dgram q[UDP_QLEN];                   /* UDP receive queue */
    int qhead, qcount;

    u32 snd_nxt, rcv_nxt;                       /* TCP */
    u8 rx[RX_RING];
    u32 rhead, rtail;
    int peer_fin;
    int pending;                                /* one unacknowledged segment (stop-and-wait) */
    u8 pend[TCP_MSS];
    u32 pend_len, pend_seq, pend_tick;
    u8 pend_flags;
    int pend_retries;
};

static struct sock *socks[NSOCK];
static u16 next_port = 49152;

static struct sock *sock_alloc(int type)
{
    for (int i = 0; i < NSOCK; i++) {
        if (!socks[i]) {
            struct sock *s = kzalloc(sizeof *s);
            if (!s)
                return NULL;
            s->type = type;
            s->timeout_ms = type == SOCK_DGRAM ? 5000 : 0;
            socks[i] = s;
            return s;
        }
    }
    return NULL;
}

static void sock_free(struct sock *s)
{
    for (int i = 0; i < NSOCK; i++)
        if (socks[i] == s)
            socks[i] = NULL;
    kfree(s);
}

static u16 pick_port(void)
{
    for (;;) {
        u16 p = next_port++;
        if (next_port < 49152)
            next_port = 49152;
        int taken = 0;
        for (int i = 0; i < NSOCK; i++)
            if (socks[i] && socks[i]->lport == p)
                taken = 1;
        if (!taken)
            return p;
    }
}

/* ---------------- Ethernet / ARP / IPv4 ---------------- */

static u8 txf[1600];
static u16 ip_id;
static const u8 bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct arp_ent {
    u32 ip;
    u8 mac[6];
    int valid;
};

static struct arp_ent arp_cache[8];
static int arp_next;
static char arp_chan;

static void arp_learn(u32 ip, const u8 *mac)
{
    for (int i = 0; i < 8; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    struct arp_ent *e = &arp_cache[arp_next++ & 7];
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->valid = 1;
}

static const u8 *arp_lookup(u32 ip)
{
    for (int i = 0; i < 8; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return arp_cache[i].mac;
    return NULL;
}

static u32 nexthop(u32 dst)
{
    return ((dst ^ cfg.ip) & cfg.mask) == 0 ? dst : cfg.gw;
}

static void eth_send_raw(const u8 *dst, u16 type, const u8 *payload, u32 len)
{
    memcpy(txf, dst, 6);
    memcpy(txf + 6, nic_mac, 6);
    put16(txf + 12, type);
    memcpy(txf + 14, payload, len);
    nic_send(txf, 14 + len);
    tx_packets++;
}

static void arp_send(u16 op, const u8 *tha, u32 tpa, const u8 *eth_dst)
{
    u8 a[28];
    put16(a, 1);                                /* Ethernet */
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, nic_mac, 6);
    put32(a + 14, cfg.ip);
    memcpy(a + 18, tha, 6);
    put32(a + 24, tpa);
    eth_send_raw(eth_dst, ETH_ARP, a, sizeof a);
}

static void arp_rx(const u8 *a, u32 len)
{
    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP)
        return;
    u16 op = get16(a + 6);
    u32 spa = get32(a + 14), tpa = get32(a + 24);
    if (spa)
        arp_learn(spa, a + 8);
    if (op == 1 && tpa == cfg.ip)
        arp_send(2, a + 8, spa, a + 8);
    else if (op == 2)
        wakeup(&arp_chan);
}

/* Builds and sends an IPv4 packet.  `mac` overrides next-hop resolution. */
static int ip_send(u32 dst, u8 proto, const u8 *payload, u32 len, const u8 *mac)
{
    if (len + 20 > 1500)
        return -EMSGSIZE;
    if (!mac) {
        mac = arp_lookup(nexthop(dst));
        if (!mac)
            return -EHOSTUNREACH;
    }
    u8 pkt[1500];
    pkt[0] = 0x45;
    pkt[1] = 0;
    put16(pkt + 2, 20 + len);
    put16(pkt + 4, ip_id++);
    put16(pkt + 6, 0x4000);                     /* don't fragment */
    pkt[8] = 64;
    pkt[9] = proto;
    put16(pkt + 10, 0);
    put32(pkt + 12, cfg.ip);
    put32(pkt + 16, dst);
    put16(pkt + 10, fold(sum_bytes(pkt, 20, 0)));
    memcpy(pkt + 20, payload, len);
    eth_send_raw(mac, ETH_IP, pkt, 20 + len);
    return 0;
}

/* Process context: make sure we know the MAC of the next hop toward `ip`. */
static int arp_ensure(u32 ip)
{
    u32 nh = nexthop(ip);
    for (int tries = 0; tries < 4; tries++) {
        if (arp_lookup(nh))
            return 0;
        arp_send(1, (const u8[6]){0}, nh, bcast_mac);
        sleep_until(&arp_chan, ticks + 50);
        if (cur->killed)
            return -EINTR;
    }
    return arp_lookup(nh) ? 0 : -EHOSTUNREACH;
}

/* ---------------- ICMP ---------------- */

static char ping_chan;
static int ping_active, ping_done;
static u16 ping_seq;
static u32 ping_start, ping_rtt;

static void icmp_rx(u32 src, const u8 *m, u32 len, const u8 *srcmac)
{
    if (len < 8)
        return;
    if (m[0] == 8) {                            /* echo request: answer it */
        u8 reply[1480];
        if (len > sizeof reply)
            return;
        memcpy(reply, m, len);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, fold(sum_bytes(reply, len, 0)));
        ip_send(src, 1, reply, len, srcmac);
    } else if (m[0] == 0 && ping_active && get16(m + 4) == 0x5043 && get16(m + 6) == ping_seq) {
        ping_rtt = ticks - ping_start;
        ping_done = 1;
        wakeup(&ping_chan);
    }
}

int net_ping(u32 ip, u32 timeout_ms)
{
    if (!net_up)
        return -ENETDOWN;
    int e = arp_ensure(ip);
    if (e < 0)
        return e;
    u8 m[8 + 32];
    m[0] = 8;
    m[1] = 0;
    put16(m + 2, 0);
    put16(m + 4, 0x5043);
    put16(m + 6, ++ping_seq);
    for (int i = 0; i < 32; i++)
        m[8 + i] = 'a' + i % 23;
    put16(m + 2, fold(sum_bytes(m, sizeof m, 0)));

    ping_active = 1;
    ping_done = 0;
    ping_start = ticks;
    u32 deadline = ticks + (timeout_ms + 9) / 10;
    ip_send(ip, 1, m, sizeof m, NULL);
    while (!ping_done) {
        if (cur->killed) {
            ping_active = 0;
            return -EINTR;
        }
        if ((i32)(ticks - deadline) >= 0) {
            ping_active = 0;
            return -ETIMEDOUT;
        }
        sleep_until(&ping_chan, deadline);
    }
    ping_active = 0;
    return ping_rtt;
}

/* ---------------- UDP ---------------- */

static void udp_rx(u32 src, const u8 *u, u32 len)
{
    if (len < 8)
        return;
    u16 sport = get16(u), dport = get16(u + 2), ulen = get16(u + 4);
    if (ulen < 8 || ulen > len)
        return;
    u32 dlen = ulen - 8;
    for (int i = 0; i < NSOCK; i++) {
        struct sock *s = socks[i];
        if (!s || s->type != SOCK_DGRAM || s->lport != dport)
            continue;
        if (s->rport && (s->rip != src || s->rport != sport))
            continue;
        if (s->qcount == UDP_QLEN || dlen > sizeof s->q[0].data)
            return;                             /* queue full: drop */
        struct dgram *d = &s->q[(s->qhead + s->qcount++) % UDP_QLEN];
        d->ip = src;
        d->port = sport;
        d->len = dlen;
        memcpy(d->data, u + 8, dlen);
        wakeup(s);
        return;
    }
}

int sock_sendto(struct sock *s, const void *buf, u32 n, u32 ip, u16 port)
{
    if (!net_up)
        return -ENETDOWN;
    if (s->type != SOCK_DGRAM)
        return -EINVAL;
    if (n > 1472)
        return -EMSGSIZE;
    int e = arp_ensure(ip);
    if (e < 0)
        return e;
    if (!s->lport)
        s->lport = pick_port();
    u8 u[8 + 1472];
    put16(u, s->lport);
    put16(u + 2, port);
    put16(u + 4, 8 + n);
    put16(u + 6, 0);                            /* checksum optional in IPv4 */
    memcpy(u + 8, buf, n);
    e = ip_send(ip, 17, u, 8 + n, NULL);
    return e < 0 ? e : (int)n;
}

int sock_recvfrom(struct sock *s, void *buf, u32 n, struct sockaddr_in *from)
{
    if (s->type != SOCK_DGRAM)
        return -EINVAL;
    if (!s->lport)
        s->lport = pick_port();
    u32 deadline = s->timeout_ms ? ticks + (s->timeout_ms + 9) / 10 : 0;
    while (!s->qcount) {
        if (cur->killed)
            return -EINTR;
        if (deadline && (i32)(ticks - deadline) >= 0)
            return -ETIMEDOUT;
        sleep_until(s, deadline);
    }
    struct dgram *d = &s->q[s->qhead];
    u32 m = d->len < n ? d->len : n;
    memcpy(buf, d->data, m);
    if (from) {
        from->ip = d->ip;
        from->port = d->port;
    }
    s->qhead = (s->qhead + 1) % UDP_QLEN;
    s->qcount--;
    return m;
}

/* ---------------- TCP ---------------- */

static u8 segbuf[24 + TCP_MSS];

static void tcp_output(struct sock *s, u8 flags, const u8 *data, u32 len, u32 seq, const u8 *mac)
{
    u8 *t = segbuf;
    u32 hdr = (flags & TCP_SYN) ? 24 : 20;
    u32 window = RX_RING - (s->rtail - s->rhead);
    put16(t, s->lport);
    put16(t + 2, s->rport);
    put32(t + 4, seq);
    put32(t + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0);
    t[12] = (hdr / 4) << 4;
    t[13] = flags;
    put16(t + 14, window > 0xFFFF ? 0xFFFF : window);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & TCP_SYN) {                      /* MSS option */
        t[20] = 2; t[21] = 4;
        put16(t + 22, TCP_MSS);
    }
    memcpy(t + hdr, data, len);

    u8 pseudo[12];
    put32(pseudo, cfg.ip);
    put32(pseudo + 4, s->rip);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(pseudo + 10, hdr + len);
    put16(t + 16, fold(sum_bytes(t, hdr + len, sum_bytes(pseudo, 12, 0))));
    ip_send(s->rip, 6, t, hdr + len, mac);
}

/* Sends a segment and keeps it for retransmission until acknowledged. */
static void tcp_queue(struct sock *s, u8 flags, const u8 *data, u32 len)
{
    memcpy(s->pend, data, len);
    s->pend_len = len;
    s->pend_flags = flags;
    s->pend_seq = s->snd_nxt;
    s->pend_tick = ticks;
    s->pend_retries = 0;
    s->pending = 1;
    tcp_output(s, flags, data, len, s->snd_nxt, NULL);
    s->snd_nxt += len + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
}

static void tcp_reset_reply(u32 src, const u8 *t, u32 len, const u8 *srcmac)
{
    u8 flags = t[13];
    if (flags & TCP_RST)
        return;
    static struct sock tmp;                     /* too big for the kernel stack; only a few fields matter */
    tmp.rhead = tmp.rtail = 0;
    tmp.lport = get16(t + 2);
    tmp.rport = get16(t);
    tmp.rip = src;
    u32 hdr = (t[12] >> 4) * 4;
    u32 plen = len > hdr ? len - hdr : 0;
    u32 seq = (flags & TCP_ACK) ? get32(t + 8) : 0;
    tmp.rcv_nxt = get32(t + 4) + plen + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
    tcp_output(&tmp, TCP_RST | TCP_ACK, NULL, 0, seq, srcmac);
}

static void tcp_fail(struct sock *s, int err)
{
    s->err = err;
    s->state = TS_DEAD;
    s->pending = 0;
    wakeup(s);
}

static void tcp_rx(u32 src, const u8 *t, u32 len, const u8 *srcmac)
{
    if (len < 20)
        return;
    u16 sport = get16(t), dport = get16(t + 2);
    u32 seq = get32(t + 4), ack = get32(t + 8);
    u32 hdr = (t[12] >> 4) * 4;
    u8 flags = t[13];
    if (hdr < 20 || hdr > len)
        return;
    const u8 *data = t + hdr;
    u32 dlen = len - hdr;

    struct sock *s = NULL;
    for (int i = 0; i < NSOCK; i++) {
        struct sock *c = socks[i];
        if (c && c->type == SOCK_STREAM && c->lport == dport && c->rport == sport && c->rip == src &&
            c->state != TS_CLOSED) {
            s = c;
            break;
        }
    }
    if (!s) {
        tcp_reset_reply(src, t, len, srcmac);
        return;
    }
    if (flags & TCP_RST) {
        if (s->closing)
            sock_free(s);
        else
            tcp_fail(s, s->state == TS_SYN_SENT ? ECONNREFUSED : ECONNRESET);
        return;
    }

    if (s->state == TS_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
            s->pending = 0;
            s->rcv_nxt = seq + 1;
            s->state = TS_ESTAB;
            tcp_output(s, TCP_ACK, NULL, 0, s->snd_nxt, srcmac);
            wakeup(s);
        }
        return;
    }

    if ((flags & TCP_ACK) && s->pending && (i32)(ack - s->snd_nxt) >= 0) {
        s->pending = 0;
        wakeup(s);
    }

    int need_ack = 0;
    if (dlen && s->state != TS_FIN_WAIT) {
        if (seq == s->rcv_nxt && dlen <= RX_RING - (s->rtail - s->rhead)) {
            for (u32 i = 0; i < dlen; i++)
                s->rx[s->rtail++ % RX_RING] = data[i];
            s->rcv_nxt += dlen;
            wakeup(s);
        }
        need_ack = 1;                           /* also re-ACKs out-of-order data */
    }
    if ((flags & TCP_FIN) && seq + dlen == s->rcv_nxt) {
        s->rcv_nxt++;
        s->peer_fin = 1;
        if (s->state == TS_ESTAB)
            s->state = TS_CLOSE_WAIT;
        need_ack = 1;
        wakeup(s);
    }
    if (need_ack)
        tcp_output(s, TCP_ACK, NULL, 0, s->snd_nxt, srcmac);

    if (s->closing) {
        if (!s->pending && s->state != TS_FIN_WAIT) {       /* data drained: send our FIN */
            s->state = TS_FIN_WAIT;
            tcp_queue(s, TCP_FIN | TCP_ACK, NULL, 0);
        } else if (!s->pending && s->peer_fin) {
            sock_free(s);                                   /* FIN acked and theirs received */
        }
    }
}

int sock_connect(struct sock *s, u32 ip, u16 port)
{
    if (!net_up)
        return -ENETDOWN;
    if (s->type == SOCK_DGRAM) {
        s->rip = ip;
        s->rport = port;
        if (!s->lport)
            s->lport = pick_port();
        return 0;
    }
    if (s->state != TS_CLOSED)
        return -EISCONN;
    int e = arp_ensure(ip);
    if (e < 0)
        return e;
    s->lport = pick_port();
    s->rip = ip;
    s->rport = port;
    s->snd_nxt = ticks * 64007u + 1000;
    s->state = TS_SYN_SENT;
    tcp_queue(s, TCP_SYN, NULL, 0);
    while (s->state == TS_SYN_SENT) {
        if (cur->killed)
            return -EINTR;
        sleep_until(s, 0);
    }
    return s->state == TS_ESTAB ? 0 : -s->err;
}

int sock_bind(struct sock *s, u16 port)
{
    for (int i = 0; i < NSOCK; i++)
        if (socks[i] && socks[i] != s && socks[i]->lport == port && socks[i]->type == s->type)
            return -EADDRINUSE;
    s->lport = port;
    return 0;
}

int sock_write(struct sock *s, const void *src, u32 n)
{
    if (s->type == SOCK_DGRAM) {
        if (!s->rport)
            return -ENOTCONN;
        return sock_sendto(s, src, n, s->rip, s->rport);
    }
    const u8 *p = src;
    u32 done = 0;
    while (done < n) {
        if (s->state == TS_DEAD)
            return done ? (int)done : -s->err;
        if (s->state != TS_ESTAB && s->state != TS_CLOSE_WAIT)
            return done ? (int)done : -ENOTCONN;
        if (s->pending) {                       /* stop-and-wait: one segment in flight */
            if (cur->killed)
                return -EINTR;
            sleep_until(s, 0);
            continue;
        }
        u32 chunk = n - done < TCP_MSS ? n - done : TCP_MSS;
        tcp_queue(s, TCP_PSH | TCP_ACK, p + done, chunk);
        done += chunk;
    }
    return done;
}

int sock_read(struct sock *s, void *dst, u32 n)
{
    if (s->type == SOCK_DGRAM)
        return sock_recvfrom(s, dst, n, NULL);
    u32 deadline = s->timeout_ms ? ticks + (s->timeout_ms + 9) / 10 : 0;
    for (;;) {
        u32 avail = s->rtail - s->rhead;
        if (avail) {
            u32 m = avail < n ? avail : n;
            u8 *out = dst;
            for (u32 i = 0; i < m; i++)
                out[i] = s->rx[s->rhead++ % RX_RING];
            if (avail >= RX_RING / 2 && s->state == TS_ESTAB)
                tcp_output(s, TCP_ACK, NULL, 0, s->snd_nxt, NULL);     /* window update */
            return m;
        }
        if (s->peer_fin)
            return 0;
        if (s->state == TS_DEAD)
            return -s->err;
        if (s->state == TS_CLOSED || s->state == TS_SYN_SENT)
            return -ENOTCONN;
        if (cur->killed)
            return -EINTR;
        if (deadline && (i32)(ticks - deadline) >= 0)
            return -ETIMEDOUT;
        sleep_until(s, deadline);
    }
}

/* Would a read return immediately (data, EOF or error)? */
int sock_ready(struct sock *s)
{
    if (s->type == SOCK_DGRAM)
        return s->qcount > 0;
    return s->rtail != s->rhead || s->peer_fin || s->state == TS_DEAD;
}

void sock_settimeout(struct sock *s, u32 ms)
{
    s->timeout_ms = ms;
}

int sock_create(int type, struct sock **out)
{
    if (type != SOCK_DGRAM && type != SOCK_STREAM)
        return -EINVAL;
    struct sock *s = sock_alloc(type);
    if (!s)
        return -ENOMEM;
    *out = s;
    return 0;
}

/* The file is gone; finish the TCP conversation in the background. */
void sock_close(struct sock *s)
{
    if (s->type == SOCK_STREAM && (s->state == TS_ESTAB || s->state == TS_CLOSE_WAIT)) {
        s->closing = 1;
        s->close_deadline = ticks + 500;
        s->rhead = s->rtail;                    /* nobody will read what's left */
        if (!s->pending) {
            s->state = TS_FIN_WAIT;
            tcp_queue(s, TCP_FIN | TCP_ACK, NULL, 0);
        }
        return;
    }
    sock_free(s);
}

/* Retransmission and cleanup timers; runs every tick from the timer IRQ. */
void net_tick(void)
{
    for (int i = 0; i < NSOCK; i++) {
        struct sock *s = socks[i];
        if (!s || s->type != SOCK_STREAM)
            continue;
        if (s->closing && ((i32)(ticks - s->close_deadline) >= 0 || s->state == TS_DEAD)) {
            sock_free(s);
            continue;
        }
        if (!s->pending || (i32)(ticks - s->pend_tick) < RTO_TICKS)
            continue;
        if (s->pend_retries >= RTX_MAX) {
            if (s->closing)
                sock_free(s);
            else
                tcp_fail(s, ETIMEDOUT);
            continue;
        }
        s->pend_retries++;
        s->pend_tick = ticks;
        tcp_output(s, s->pend_flags, s->pend, s->pend_len, s->pend_seq, NULL);
    }
}

/* ---------------- receive dispatch ---------------- */

static void ip_rx(const u8 *p, u32 len, const u8 *srcmac)
{
    if (len < 20 || (p[0] >> 4) != 4)
        return;
    u32 ihl = (p[0] & 0xF) * 4;
    u32 total = get16(p + 2);
    if (ihl < 20 || total < ihl || total > len)
        return;
    if (get16(p + 6) & 0x3FFF)
        return;                                 /* fragments are not supported */
    u32 src = get32(p + 12), dst = get32(p + 16);
    if (dst != cfg.ip && dst != 0xFFFFFFFFu)
        return;
    const u8 *body = p + ihl;
    u32 blen = total - ihl;
    switch (p[9]) {
    case 1:  icmp_rx(src, body, blen, srcmac); break;
    case 6:  tcp_rx(src, body, blen, srcmac); break;
    case 17: udp_rx(src, body, blen); break;
    }
}

void net_rx(const u8 *f, u32 len)
{
    if (len < 14)
        return;
    rx_packets++;
    switch (get16(f + 12)) {
    case ETH_ARP: arp_rx(f + 14, len - 14); break;
    case ETH_IP:  ip_rx(f + 14, len - 14, f + 6); break;
    }
}

void net_info(struct netinfo *ni)
{
    memcpy(ni->mac, nic_mac, 6);
    ni->up = net_up;
    ni->pad = 0;
    ni->ip = cfg.ip;
    ni->mask = cfg.mask;
    ni->gw = cfg.gw;
    ni->dns = cfg.dns;
    ni->rx_packets = rx_packets;
    ni->tx_packets = tx_packets;
}

void net_init(void)
{
    int e = nic_init();
    if (e < 0) {
        kprintf("[net ] no supported network card (%d)\n", e);
        return;
    }
    net_up = 1;
    kprintf("[net ] rtl8139 %02x:%02x:%02x:%02x:%02x:%02x irq %d, %d.%d.%d.%d\n", nic_mac[0],
            nic_mac[1], nic_mac[2], nic_mac[3], nic_mac[4], nic_mac[5], nic_irq_line,
            cfg.ip >> 24, (cfg.ip >> 16) & 255, (cfg.ip >> 8) & 255, cfg.ip & 255);
}
