#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include "plat.h"

#define VIRTIO_NET_BASE   0x10002000u
#define VIRTIO_NET_SIZE   0x1000u

/* A console with no network stack at all - the GameCube, which needs a
   Broadband Adapter nobody can assume - compiles this device out entirely
   rather than linking against socket calls its libc does not have. The
   device then simply never answers, so the guest's virtio-mmio probe skips
   the node exactly as it does when the user turns networking off. */
#ifndef PLAT_HAS_NET

static struct { bool soc_ready; } vnet;

static inline void     vnet_init(void)  { vnet.soc_ready = false; }
static inline void     vnet_poll(uint8_t *ram) { (void)ram; }
static inline uint32_t vnet_load(uint32_t addy) { (void)addy; return 0; }
static inline void     vnet_store(uint32_t addy, uint32_t val, uint8_t *ram) {
    (void)addy; (void)val; (void)ram;
}

#else

#include "plat_sock.h"

/*
 * Virtio-net device backed by a small userspace NAT ("slirp"), the same
 * technique QEMU's -netdev user uses: the guest gets a private virtual
 * subnet (10.0.2.0/24) with a synthetic gateway/DHCP/DNS server at
 * 10.0.2.2, and every UDP/TCP flow the guest opens is transparently
 * proxied 1:1 onto a real BSD socket on the console's own stack, which
 * talks out over whatever network (WiFi) the 3DS is actually connected
 * to. No root/raw sockets are needed because we never forge source
 * addresses on the real wire — we just terminate+relay each flow.
 *
 * Known limitations (documented rather than silently broken):
 *  - ICMP echo is only answered for the gateway address itself (a
 *    reachability/latency check of the proxy, not of the internet).
 *    Real ping to internet hosts needs raw sockets, which homebrew
 *    doesn't get, so it is intentionally not faked.
 *  - TCP has no retransmission of our own segments: the guest<->device
 *    "wire" is just memcpy into guest RAM, so it never drops frames
 *    once we've matched to a slot; the only backpressure is RX ring
 *    space, which we handle by holding data until buffers are posted.
 *  - Fixed-size flow tables (8 TCP, 12 UDP) — oldest idle flow is
 *    evicted to make room.
 */


#define VNET_MTU          1500
#define VNET_FRAME_MAX    1536
/* With VIRTIO_F_VERSION_1 the net header is always the 12-byte
   virtio_net_hdr_mrg_rxbuf layout (num_buffers present even without
   MRG_RXBUF - the Linux driver picks 12 whenever VERSION_1 is set).
   Getting this wrong shifts every frame by 2 bytes in both directions,
   which silently fails the ethertype check on TX and misframes RX. */
#define VNET_HDR_LEN      12

#define GUEST_IP_STR      "10.0.2.15"
#define GW_IP_STR         "10.0.2.2"
#define DNS1_IP_STR       "1.1.1.1"
#define DNS2_IP_STR       "8.8.8.8"

static const uint8_t vnet_guest_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x02};
static const uint8_t vnet_gw_mac[6]    = {0x52,0x54,0x00,0x12,0x34,0x01};

static uint32_t vnet_ip_guest, vnet_ip_gw, vnet_ip_dns1, vnet_ip_dns2, vnet_ip_bcast;

/* ------------------------------------------------------------------ */
/* virtio-mmio transport state (2 queues: 0=RX to guest, 1=TX from guest) */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t queue_num;
    uint32_t queue_ready;
    uint32_t queue_desc_lo;
    uint32_t queue_driver_lo;
    uint32_t queue_device_lo;
    uint16_t last_avail_idx;
} vnet_queue_t;

static struct {
    uint32_t dev_feat_sel, drv_feat_sel;
    uint32_t queue_sel;
    uint32_t int_status;
    uint32_t status;
    vnet_queue_t q[2];
    bool soc_ready;
} vnet;

/* Pending frames waiting to be handed to the guest's RX queue */
typedef struct { uint16_t len; uint8_t data[VNET_FRAME_MAX]; } vnet_pend_t;
#define VNET_PEND_N 24
static vnet_pend_t vnet_pend[VNET_PEND_N];
static int vnet_pend_head = 0, vnet_pend_tail = 0;

static bool vnet_pend_push(const uint8_t *data, uint16_t len) {
    int nh = (vnet_pend_head + 1) % VNET_PEND_N;
    if (nh == vnet_pend_tail) return false; /* full, drop */
    if (len > VNET_FRAME_MAX) len = VNET_FRAME_MAX;
    vnet_pend[vnet_pend_head].len = len;
    memcpy(vnet_pend[vnet_pend_head].data, data, len);
    vnet_pend_head = nh;
    return true;
}

/* ------------------------------------------------------------------ */
/* checksum helpers                                                    */
/* ------------------------------------------------------------------ */
static uint32_t cksum_add(uint32_t sum, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    return sum;
}
static uint16_t cksum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ------------------------------------------------------------------ */
/* Ethernet/IP frame builder — writes into `out`, returns total length */
/* ------------------------------------------------------------------ */
static int build_eth_ip(uint8_t *out, const uint8_t *dst_mac, const uint8_t *src_mac,
                         uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                         const uint8_t *payload, int paylen) {
    memcpy(out + 0, dst_mac, 6);
    memcpy(out + 6, src_mac, 6);
    out[12] = 0x08; out[13] = 0x00; /* IPv4 */

    uint8_t *ip = out + 14;
    int totlen = 20 + paylen;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = totlen >> 8; ip[3] = totlen & 0xff;
    ip[4] = 0; ip[5] = 0;             /* id */
    ip[6] = 0x40; ip[7] = 0;          /* DF, no frag */
    ip[8] = 64;                       /* ttl */
    ip[9] = proto;
    ip[10] = 0; ip[11] = 0;           /* csum placeholder */
    uint32_t sbe = htonl(src_ip), dbe = htonl(dst_ip);
    g_st32(ip + 12, sbe);
    g_st32(ip + 16, dbe);
    uint16_t csum = cksum_fold(cksum_add(0, ip, 20));
    ip[10] = csum >> 8; ip[11] = csum & 0xff;

    memcpy(ip + 20, payload, paylen);
    return 14 + 20 + paylen;
}

static void queue_ip_frame(const uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip,
                            uint8_t proto, const uint8_t *payload, int paylen) {
    static uint8_t frame[VNET_HDR_LEN + VNET_FRAME_MAX];
    memset(frame, 0, VNET_HDR_LEN); /* virtio_net_hdr: all zero = no offload */
    int flen = build_eth_ip(frame + VNET_HDR_LEN, dst_mac, vnet_gw_mac,
                             src_ip, dst_ip, proto, payload, paylen);
    vnet_pend_push(frame, VNET_HDR_LEN + flen);
}

/* ------------------------------------------------------------------ */
/* ARP: only answer "who has gateway?"                                 */
/* ------------------------------------------------------------------ */
static void vnet_handle_arp(const uint8_t *pkt, int len) {
    if (len < 28) return;
    uint16_t oper = (pkt[6] << 8) | pkt[7];
    uint32_t tpa; tpa = g_ld32(pkt + 24); tpa = ntohl(tpa);
    if (oper != 1 /* request */ || tpa != vnet_ip_gw) return;

    static uint8_t frame[VNET_HDR_LEN + 42];
    memset(frame, 0, VNET_HDR_LEN);
    uint8_t *e = frame + VNET_HDR_LEN;
    memcpy(e + 0, pkt + 8, 6);       /* dst = requester mac */
    memcpy(e + 6, vnet_gw_mac, 6);
    e[12] = 0x08; e[13] = 0x06;      /* ARP */
    uint8_t *a = e + 14;
    a[0]=0; a[1]=1; a[2]=0x08; a[3]=0; a[4]=6; a[5]=4;
    a[6]=0; a[7]=2; /* reply */
    memcpy(a + 8, vnet_gw_mac, 6);
    uint32_t gwbe = htonl(vnet_ip_gw); g_st32(a + 14, gwbe);
    memcpy(a + 18, pkt + 8, 6);      /* target = requester */
    memcpy(a + 24, pkt + 14, 4);     /* target ip = requester's spa (offset 14 in arp) */
    vnet_pend_push(frame, VNET_HDR_LEN + 42);
}

/* ------------------------------------------------------------------ */
/* DHCP: single static lease for the guest                             */
/* ------------------------------------------------------------------ */
static void vnet_send_dhcp(const uint8_t *req, int reqlen, uint8_t msg_type, const uint8_t *cmac) {
    static uint8_t rep[300];
    memset(rep, 0, sizeof(rep));
    rep[0] = 2;   /* BOOTREPLY */
    rep[1] = 1; rep[2] = 6; rep[3] = 0;
    memcpy(rep + 4, req + 4, 4);           /* xid */
    uint32_t yiaddr = htonl(vnet_ip_guest);
    g_st32(rep + 16, yiaddr);
    uint32_t siaddr = htonl(vnet_ip_gw);
    g_st32(rep + 20, siaddr);
    memcpy(rep + 28, cmac, 6);
    rep[236]=99; rep[237]=130; rep[238]=83; rep[239]=99; /* magic cookie */
    int o = 240;
    rep[o++] = 53; rep[o++] = 1; rep[o++] = msg_type;            /* DHCP msg type */
    rep[o++] = 54; rep[o++] = 4; { uint32_t v=htonl(vnet_ip_gw); g_st32(rep+o, v); o+=4; } /* server id */
    rep[o++] = 1;  rep[o++] = 4; { uint32_t v=htonl(0xffffff00u); g_st32(rep+o, v); o+=4; } /* subnet */
    rep[o++] = 3;  rep[o++] = 4; { uint32_t v=htonl(vnet_ip_gw); g_st32(rep+o, v); o+=4; } /* router */
    rep[o++] = 6;  rep[o++] = 8;
      { uint32_t v=htonl(vnet_ip_dns1); g_st32(rep+o, v); o+=4; }
      { uint32_t v=htonl(vnet_ip_dns2); g_st32(rep+o, v); o+=4; }
    rep[o++] = 51; rep[o++] = 4; { uint32_t v=htonl(86400u); g_st32(rep+o, v); o+=4; } /* lease */
    rep[o++] = 255;
    int udplen = 8 + o;
    static uint8_t udp[8 + 300];
    udp[0]=0; udp[1]=67; udp[2]=0; udp[3]=68;
    udp[4]=udplen>>8; udp[5]=udplen&0xff;
    udp[6]=0; udp[7]=0;
    memcpy(udp + 8, rep, o);
    queue_ip_frame(cmac, vnet_ip_gw, 0xffffffffu, 17 /* UDP */, udp, udplen);
    (void)reqlen;
}

static void vnet_handle_udp(const uint8_t *eth, const uint8_t *ip, const uint8_t *udp, int udplen);

static void vnet_handle_dhcp(const uint8_t *eth, const uint8_t *bootp, int len) {
    if (len < 240 || bootp[0] != 1) return; /* BOOTREQUEST only */
    const uint8_t *opts = bootp + 240;
    int olen = len - 240;
    uint8_t mtype = 0;
    for (int i = 0; i + 1 < olen; ) {
        uint8_t code = opts[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        uint8_t l = opts[i+1];
        if (code == 53 && l >= 1) mtype = opts[i+2];
        i += 2 + l;
    }
    uint8_t reply = (mtype == 3 /* REQUEST */) ? 5 /* ACK */ : 2 /* OFFER */;
    vnet_send_dhcp(bootp, len, reply, eth + 6);
}

/* ------------------------------------------------------------------ */
/* ICMP echo — gateway only                                            */
/* ------------------------------------------------------------------ */
static void vnet_handle_icmp(const uint8_t *smac, uint32_t src_ip, const uint8_t *icmp, int len) {
    if (len < 8 || icmp[0] != 8 /* echo request */ || src_ip == 0) return;
    if (len > 128) len = 128;
    static uint8_t rep[128];
    memcpy(rep, icmp, len);
    rep[0] = 0; /* echo reply */
    rep[2] = 0; rep[3] = 0;
    uint16_t c = cksum_fold(cksum_add(0, rep, len));
    rep[2] = c >> 8; rep[3] = c & 0xff;
    queue_ip_frame(smac, vnet_ip_gw, src_ip, 1 /* ICMP */, rep, len);
}

/* ------------------------------------------------------------------ */
/* UDP NAT                                                             */
/* ------------------------------------------------------------------ */
#define UDP_MAX_FLOWS 12
typedef struct {
    bool used;
    int fd;
    uint16_t guest_port;
    uint32_t dst_ip; uint16_t dst_port;
    uint8_t guest_mac[6];
    uint64_t last_tick;
} udp_flow_t;
static udp_flow_t udp_flows[UDP_MAX_FLOWS];

static udp_flow_t *udp_find_or_create(uint16_t guest_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *gmac) {
    int oldest = -1; uint64_t oldest_tick = ~0ull;
    for (int i = 0; i < UDP_MAX_FLOWS; i++) {
        if (udp_flows[i].used && udp_flows[i].guest_port == guest_port &&
            udp_flows[i].dst_ip == dst_ip && udp_flows[i].dst_port == dst_port)
            return &udp_flows[i];
        if (!udp_flows[i].used) { oldest = i; oldest_tick = 0; break; }
        if (udp_flows[i].last_tick < oldest_tick) { oldest_tick = udp_flows[i].last_tick; oldest = i; }
    }
    if (oldest < 0) return NULL;
    if (udp_flows[oldest].used) closesocket(udp_flows[oldest].fd);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;
    PLAT_SOCK_SET_NONBLOCK(fd);
    udp_flow_t *f = &udp_flows[oldest];
    f->used = true; f->fd = fd; f->guest_port = guest_port;
    f->dst_ip = dst_ip; f->dst_port = dst_port;
    memcpy(f->guest_mac, gmac, 6);
    f->last_tick = plat_us();
    return f;
}

static void vnet_handle_udp(const uint8_t *eth, const uint8_t *ip, const uint8_t *udp, int udplen) {
    if (udplen < 8) return;
    uint32_t src_ip; src_ip = g_ld32(ip + 12); src_ip = ntohl(src_ip);
    uint32_t dst_ip; dst_ip = g_ld32(ip + 16); dst_ip = ntohl(dst_ip);
    uint16_t sport = (udp[0]<<8)|udp[1], dport = (udp[2]<<8)|udp[3];
    const uint8_t *payload = udp + 8;
    int paylen = udplen - 8;

    if (dport == 67 && dst_ip == 0xffffffffu) { vnet_handle_dhcp(eth, payload, paylen); return; }
    if (src_ip != vnet_ip_guest) return;

    udp_flow_t *f = udp_find_or_create(sport, dst_ip, dport, eth + 6);
    if (!f) return;
    f->last_tick = plat_us();

    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(f->dst_port);
    sa.sin_addr.s_addr = htonl(f->dst_ip);
    sendto(f->fd, payload, paylen, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static void udp_poll(void) {
    for (int i = 0; i < UDP_MAX_FLOWS; i++) {
        udp_flow_t *f = &udp_flows[i];
        if (!f->used) continue;
        if (plat_us() - f->last_tick > 120ull * 1000000ull) { /* ~120s idle */
            closesocket(f->fd); f->used = false; continue;
        }
        for (;;) {
            static uint8_t buf[1500];
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int n = recvfrom(f->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            uint16_t udplen = 8 + n;
            static uint8_t udp[8 + 1500];
            udp[0]=f->dst_port>>8; udp[1]=f->dst_port&0xff;
            udp[2]=f->guest_port>>8; udp[3]=f->guest_port&0xff;
            udp[4]=udplen>>8; udp[5]=udplen&0xff;
            udp[6]=0; udp[7]=0;
            memcpy(udp+8, buf, n);
            queue_ip_frame(f->guest_mac, ntohl(from.sin_addr.s_addr), vnet_ip_guest, 17, udp, udplen);
            f->last_tick = plat_us();
        }
    }
}

/* ------------------------------------------------------------------ */
/* TCP NAT — minimal proxy state machine                               */
/* ------------------------------------------------------------------ */
#define TCP_MAX_CONNS 6
/* TCP_SYN_GUEST: an inbound (host->guest) connection we've accepted on the
   ssh forward port and are now handshaking into the guest - we sent the SYN
   and are waiting for the guest's SYN-ACK. */
typedef enum { TCP_FREE, TCP_CONNECTING, TCP_SYN_GUEST, TCP_ESTABLISHED, TCP_CLOSING, TCP_CLOSED } tcp_st_t;
typedef struct {
    tcp_st_t state;
    int fd;
    uint16_t guest_port, dst_port;
    uint32_t dst_ip;
    uint8_t guest_mac[6];
    uint32_t seq_us;      /* next seq we send (device -> guest) */
    uint32_t ack_them;    /* next byte we expect from guest */
    uint64_t last_tick;
    bool guest_fin_seen;
    bool local_fin_sent;
    uint32_t syn_retries;
} tcp_conn_t;
static tcp_conn_t tcp_conns[TCP_MAX_CONNS];

/* Inbound ssh port forward (QEMU hostfwd-style): listen on the 3DS side and
   proxy accepted connections to the guest's dropbear. */
#define VNET_SSH_FWD_PORT   2222
#define VNET_SSH_GUEST_PORT 22
static int ssh_listen_fd = -1;

static void tcp_send_seg(tcp_conn_t *c, uint8_t flags, const uint8_t *payload, int paylen) {
    uint8_t seg[20 + 1460];
    seg[0]=c->dst_port>>8; seg[1]=c->dst_port&0xff;
    seg[2]=c->guest_port>>8; seg[3]=c->guest_port&0xff;
    uint32_t seq = htonl(c->seq_us); g_st32(seg+4, seq);
    uint32_t ack = htonl(c->ack_them); g_st32(seg+8, ack);
    seg[12] = (5 << 4); seg[13] = flags;
    seg[14] = 0xff; seg[15] = 0xff; /* window */
    seg[16]=0; seg[17]=0; seg[18]=0; seg[19]=0;
    if (paylen > 1460) paylen = 1460;
    if (paylen) memcpy(seg + 20, payload, paylen);

    /* pseudo-header checksum */
    uint32_t sum = 0;
    uint32_t sbe = htonl(c->dst_ip), dbe = htonl(vnet_ip_guest);
    sum = cksum_add(sum, &sbe, 4);
    sum = cksum_add(sum, &dbe, 4);
    uint8_t ph[4] = {0, 6, (uint8_t)((20+paylen)>>8), (uint8_t)((20+paylen)&0xff)};
    sum = cksum_add(sum, ph, 4);
    sum = cksum_add(sum, seg, 20 + paylen);
    uint16_t c16 = cksum_fold(sum);
    seg[16] = c16 >> 8; seg[17] = c16 & 0xff;

    queue_ip_frame(c->guest_mac, c->dst_ip, vnet_ip_guest, 6 /* TCP */, seg, 20 + paylen);
    if (flags & 0x02) c->seq_us += 1; /* SYN consumes a sequence number */
    if (flags & 0x01) c->seq_us += 1; /* FIN consumes a sequence number */
    c->seq_us += paylen;
    c->last_tick = plat_us();
}

static tcp_conn_t *tcp_find(uint16_t guest_port, uint32_t dst_ip, uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++)
        if (tcp_conns[i].state != TCP_FREE && tcp_conns[i].guest_port == guest_port &&
            tcp_conns[i].dst_ip == dst_ip && tcp_conns[i].dst_port == dst_port)
            return &tcp_conns[i];
    return NULL;
}

static void tcp_close_slot(tcp_conn_t *c) {
    if (c->fd >= 0) closesocket(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void vnet_handle_tcp(const uint8_t *eth, const uint8_t *ip, const uint8_t *tcp, int tcplen) {
    if (tcplen < 20) return;
    uint32_t src_ip; src_ip = g_ld32(ip + 12); src_ip = ntohl(src_ip);
    uint32_t dst_ip; dst_ip = g_ld32(ip + 16); dst_ip = ntohl(dst_ip);
    if (src_ip != vnet_ip_guest) return;
    uint16_t sport = (tcp[0]<<8)|tcp[1], dport = (tcp[2]<<8)|tcp[3];
    uint32_t seq; seq = g_ld32(tcp+4); seq = ntohl(seq);
    uint8_t doff = (tcp[12] >> 4) * 4;
    uint8_t flags = tcp[13];
    const uint8_t *payload = tcp + doff;
    int paylen = tcplen - doff;
    if (paylen < 0) paylen = 0;

    tcp_conn_t *c = tcp_find(sport, dst_ip, dport);

    if (flags & 0x04 /* RST */) { if (c) tcp_close_slot(c); return; }

    if ((flags & 0x02) && !c) { /* new SYN */
        int slot = -1;
        for (int i = 0; i < TCP_MAX_CONNS; i++) if (tcp_conns[i].state == TCP_FREE) { slot = i; break; }
        if (slot < 0) { /* evict oldest */
            uint64_t oldest = ~0ull;
            for (int i = 0; i < TCP_MAX_CONNS; i++) if (tcp_conns[i].last_tick < oldest) { oldest = tcp_conns[i].last_tick; slot = i; }
            tcp_close_slot(&tcp_conns[slot]);
        }
        c = &tcp_conns[slot];
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        PLAT_SOCK_SET_NONBLOCK(fd);
        struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET; sa.sin_port = htons(dport);
        sa.sin_addr.s_addr = htonl(dst_ip);
        connect(fd, (struct sockaddr *)&sa, sizeof(sa)); /* EINPROGRESS expected */
        c->state = TCP_CONNECTING;
        c->fd = fd;
        c->guest_port = sport; c->dst_port = dport; c->dst_ip = dst_ip;
        memcpy(c->guest_mac, eth + 6, 6);
        c->seq_us = (uint32_t)plat_us();
        c->ack_them = seq + 1;
        c->last_tick = plat_us();
        /* Optimistic SYN-ACK: assume connect succeeds; RST later if not. */
        tcp_send_seg(c, 0x12 /* SYN|ACK */, NULL, 0);
        return;
    }
    if (!c) return;

    if (c->state == TCP_SYN_GUEST) {
        /* Waiting for the guest's SYN-ACK to our forwarded-connection SYN. */
        if ((flags & 0x12) == 0x12) {
            c->ack_them = seq + 1;
            c->state = TCP_ESTABLISHED;
            tcp_send_seg(c, 0x10 /* ACK */, NULL, 0);
        }
        return;
    }

    c->last_tick = plat_us();
    if (paylen > 0) {
        if (c->fd >= 0) send(c->fd, payload, paylen, 0);
        c->ack_them = seq + paylen;
        tcp_send_seg(c, 0x10 /* ACK */, NULL, 0); /* ack data */
    }
    if (flags & 0x01 /* FIN */) {
        c->ack_them = seq + paylen + 1;
        c->guest_fin_seen = true;
        tcp_send_seg(c, 0x10, NULL, 0);
        if (c->fd >= 0) shutdown(c->fd, SHUT_WR);
        if (c->local_fin_sent) tcp_close_slot(c);
    }
}

static void tcp_poll(void) {
    /* Accept inbound ssh-forward connections and start handshaking them
       into the guest. The synthetic remote is the gateway IP (so the
       guest's ARP already resolves it) with a rotating ephemeral port. */
    if (ssh_listen_fd >= 0) {
        for (;;) {
            /* Real storage rather than the NULL/NULL POSIX allows: wut's
               socket layer dereferences the length unconditionally, so on the
               Wii U this took the app down on the first poll after the network
               came up - which read as the guest crashing at boot. Nothing here
               wants the peer address; it is written and ignored. */
            struct sockaddr_in peer;
            socklen_t peerlen = sizeof(peer);
            int afd = accept(ssh_listen_fd, (struct sockaddr *)&peer, &peerlen);
            if (afd < 0) break;
            int slot = -1;
            for (int i = 0; i < TCP_MAX_CONNS; i++)
                if (tcp_conns[i].state == TCP_FREE) { slot = i; break; }
            if (slot < 0) { closesocket(afd); break; }
            PLAT_SOCK_SET_NONBLOCK(afd);
            static uint16_t fwd_port_seq = 0;
            tcp_conn_t *c = &tcp_conns[slot];
            c->state = TCP_SYN_GUEST;
            c->fd = afd;
            c->guest_port = VNET_SSH_GUEST_PORT;
            c->dst_ip = vnet_ip_gw;
            c->dst_port = (uint16_t)(40000u + (fwd_port_seq++ & 0x1fff));
            memcpy(c->guest_mac, vnet_guest_mac, 6);
            c->seq_us = (uint32_t)plat_us();
            c->ack_them = 0;
            c->guest_fin_seen = c->local_fin_sent = false;
            c->syn_retries = 0;
            tcp_send_seg(c, 0x02 /* SYN */, NULL, 0);
        }
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &tcp_conns[i];
        if (c->state == TCP_FREE) continue;
        if (plat_us() - c->last_tick > 300ull * 1000000ull) { tcp_close_slot(c); continue; }

        if (c->state == TCP_SYN_GUEST) {
            /* Retransmit our SYN until the guest answers (it may still be
               booting); give up after ~30 tries. tcp_send_seg advanced
               seq_us for the SYN, so rewind before resending the same one. */
            if (plat_us() - c->last_tick > 1000ull * 1000ull) {
                if (++c->syn_retries > 30) { tcp_close_slot(c); continue; }
                c->seq_us -= 1;
                tcp_send_seg(c, 0x02, NULL, 0);
            }
            continue;
        }

        if (c->state == TCP_CONNECTING) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            fd_set wfds; FD_ZERO(&wfds); FD_SET(c->fd, &wfds);
            struct timeval tv = {0,0};
            int r = select(c->fd + 1, NULL, &wfds, NULL, &tv);
            if (err != 0) { tcp_send_seg(c, 0x14 /* RST|ACK */, NULL, 0); tcp_close_slot(c); continue; }
            if (r > 0 && FD_ISSET(c->fd, &wfds)) c->state = TCP_ESTABLISHED;
            continue;
        }
        if (c->state != TCP_ESTABLISHED) continue;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(c->fd, &rfds);
        struct timeval tv = {0,0};
        if (select(c->fd + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(c->fd, &rfds)) {
            uint8_t buf[1460];
            int n = recv(c->fd, buf, sizeof(buf), 0);
            if (n > 0) {
                tcp_send_seg(c, 0x18 /* PSH|ACK */, buf, n);
            } else if (n == 0) {
                c->local_fin_sent = true;
                tcp_send_seg(c, 0x11 /* FIN|ACK */, NULL, 0);
                if (c->guest_fin_seen) tcp_close_slot(c);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* TX path: guest -> device                                            */
/* ------------------------------------------------------------------ */
static void vnet_dispatch_frame(const uint8_t *f, int len) {
    if (len < 14) return;
    uint16_t ethtype = (f[12] << 8) | f[13];
    const uint8_t *l3 = f + 14;
    int l3len = len - 14;
    if (ethtype == 0x0806) { vnet_handle_arp(l3, l3len); return; }
    if (ethtype != 0x0800 || l3len < 20) return;
    uint8_t ihl = (l3[0] & 0x0f) * 4;
    uint8_t proto = l3[9];
    const uint8_t *l4 = l3 + ihl;
    int l4len = l3len - ihl;
    if (proto == 17) vnet_handle_udp(f, l3, l4, l4len);
    else if (proto == 6) vnet_handle_tcp(f, l3, l4, l4len);
    else if (proto == 1) {
        uint32_t src_ip; src_ip = g_ld32(l3 + 12); src_ip = ntohl(src_ip);
        vnet_handle_icmp(f + 6, src_ip, l4, l4len);
    }
}

static void vnet_process_tx(uint8_t *ram) {
    vnet_queue_t *q = &vnet.q[1];
    if (!q->queue_ready || !q->queue_desc_lo) return;
    uint8_t *desc_table = guest_ptr(ram, q->queue_desc_lo, VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, q->queue_driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, q->queue_device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    uint16_t avail_idx; avail_idx = g_ld16(avail_ring + 2);
    while (q->last_avail_idx != avail_idx) {
        uint16_t ring_pos = q->last_avail_idx % VQUEUE_SIZE;
        uint16_t head; head = g_ld16(avail_ring + 4u + ring_pos * 2u);
        q->last_avail_idx++;

        static uint8_t frame[VNET_FRAME_MAX];
        int flen = 0;
        int skip = VNET_HDR_LEN; /* skip virtio_net_hdr bytes from the front of the chain */
        uint16_t di = head;
        for (int guard = 0; guard < 64; guard++) {
            uint64_t a; uint32_t l; uint16_t fl, nx;
            vblk_read_desc(desc_table, di, &a, &l, &fl, &nx);
            uint8_t *buf = guest_ptr(ram, (uint32_t)a, l);
            if (buf) {
                uint32_t off = 0;
                if (skip > 0) {
                    uint32_t s = (uint32_t)skip < l ? (uint32_t)skip : l;
                    skip -= s; off = s;
                }
                uint32_t copyable = (uint32_t)l - off;
                if (copyable > 0 && flen + (int)copyable <= (int)sizeof(frame)) {
                    memcpy(frame + flen, buf + off, copyable);
                    flen += copyable;
                }
            }
            if (!(fl & VDESC_F_NEXT)) break;
            di = nx;
        }
        if (flen > 0) vnet_dispatch_frame(frame, flen);

        uint16_t used_idx_val; used_idx_val = g_ld16(used_ring + 2);
        uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
        uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
        uint32_t hd32 = head, wl = flen;
        g_st32(ue + 0, hd32); g_st32(ue + 4, wl);
        used_idx_val++; g_st16(used_ring + 2, used_idx_val);
        vnet.int_status |= 1u;
    }
}

/* RX path: device -> guest, drained from vnet_pend[] */
static void vnet_pump_rx(uint8_t *ram) {
    vnet_queue_t *q = &vnet.q[0];
    if (!q->queue_ready || !q->queue_desc_lo) return;
    uint8_t *desc_table = guest_ptr(ram, q->queue_desc_lo, VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, q->queue_driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, q->queue_device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    uint16_t avail_idx; avail_idx = g_ld16(avail_ring + 2);
    while (vnet_pend_tail != vnet_pend_head && q->last_avail_idx != avail_idx) {
        uint16_t ring_pos = q->last_avail_idx % VQUEUE_SIZE;
        uint16_t head; head = g_ld16(avail_ring + 4u + ring_pos * 2u);

        vnet_pend_t *p = &vnet_pend[vnet_pend_tail];
        uint32_t written = 0;
        uint16_t di = head;
        for (int guard = 0; guard < 64 && written < p->len; guard++) {
            uint64_t a; uint32_t l; uint16_t fl, nx;
            vblk_read_desc(desc_table, di, &a, &l, &fl, &nx);
            uint8_t *buf = guest_ptr(ram, (uint32_t)a, l);
            if (buf) {
                uint32_t n = p->len - written;
                if (n > l) n = l;
                memcpy(buf, p->data + written, n);
                written += n;
            }
            if (!(fl & VDESC_F_NEXT)) break;
            di = nx;
        }
        q->last_avail_idx++;
        vnet_pend_tail = (vnet_pend_tail + 1) % VNET_PEND_N;

        uint16_t used_idx_val; used_idx_val = g_ld16(used_ring + 2);
        uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
        uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
        uint32_t hd32 = head;
        g_st32(ue + 0, hd32); g_st32(ue + 4, written);
        used_idx_val++; g_st16(used_ring + 2, used_idx_val);
        vnet.int_status |= 1u;
    }
}

/* Called every frame from the main loop: pumps sockets and drains queues.
   Only the real-socket flows need SOC; the synthetic gateway services
   (ARP, DHCP, gateway ICMP) and RX delivery must run even without it,
   or the guest can never even get its address. */
static void vnet_poll(uint8_t *ram) {
    if (vnet.soc_ready) {
        udp_poll();
        tcp_poll();
    }
    vnet_pump_rx(ram);
    if (vnet.int_status) plic_set_pending(PLIC_SRC_NET, true);
}

/* ------------------------------------------------------------------ */
/* virtio-mmio register interface                                      */
/* ------------------------------------------------------------------ */
static void vnet_init(void) {
    memset(&vnet, 0, sizeof(vnet));
    vnet.q[0].queue_num = VQUEUE_SIZE;
    vnet.q[1].queue_num = VQUEUE_SIZE;
    for (int i = 0; i < TCP_MAX_CONNS; i++) tcp_conns[i].fd = -1;

    struct in_addr a;
    inet_aton(GUEST_IP_STR, &a); vnet_ip_guest = ntohl(a.s_addr);
    inet_aton(GW_IP_STR, &a);    vnet_ip_gw    = ntohl(a.s_addr);
    inet_aton(DNS1_IP_STR, &a);  vnet_ip_dns1  = ntohl(a.s_addr);
    inet_aton(DNS2_IP_STR, &a);  vnet_ip_dns2  = ntohl(a.s_addr);
    vnet_ip_bcast = 0xffffffffu;

    vnet.soc_ready = plat_net_init();

    /* ssh port forward: connections to <3DS ip>:2222 land on the guest's
       dropbear. Failure is non-fatal - outbound NAT still works. */
    if (vnet.soc_ready && ssh_listen_fd < 0) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd >= 0) {
            int one = 1;
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(VNET_SSH_FWD_PORT);
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(lfd, 2) == 0) {
                PLAT_SOCK_SET_NONBLOCK(lfd);
                ssh_listen_fd = lfd;
            } else {
                closesocket(lfd);
            }
        }
    }
}

/* Config space: the 6-byte MAC, read a byte at a time by the driver.
   noinline for the same reason as v9p_config_read in virtio_9p.h: inlined,
   the index here is `addy - 0x10002100`, and GCC is free to strength-reduce
   the lookup into `*((&vnet_guest_mac - 0x10002100) + addy)` and store that
   folded base in a literal pool. It wraps to 0xF0xxxxxx, which a 3DSX
   absolute relocation cannot encode — the top nibble is a reserved sub-type
   field — and every loader then rejects the entire app with a relocation
   error and no useful diagnostic. That exact fold in the 9P device cost a
   long bisect to track down. GCC happens not to make that choice here today;
   this makes sure it stays that way. `make` runs tools/check3dsx.py to catch
   it if some future change reintroduces the pattern elsewhere. */
static __attribute__((noinline)) uint32_t vnet_config_read(uint32_t o) {
    if (o < sizeof(vnet_guest_mac)) return vnet_guest_mac[o];
    return 0;
}

static uint32_t vnet_load(uint32_t addy) {
    uint32_t r = addy - VIRTIO_NET_BASE;
    if (r >= 0x100) return vnet_config_read(r - 0x100);
    switch (r) {
    case VREG_MAGIC:           return 0x74726976u;
    case VREG_VERSION:         return 2u;
    case VREG_DEVICE_ID:       return 1u; /* network card */
    case VREG_VENDOR_ID:       return 0x554d4551u;
    case VREG_DEVICE_FEATURES:
        if (vnet.dev_feat_sel == 0) return (1u << 5); /* VIRTIO_NET_F_MAC */
        return VIRTIO_F_VERSION_1_HI;
    case VREG_QUEUE_NUM_MAX:   return VQUEUE_SIZE;
    case VREG_QUEUE_READY:     return vnet.q[vnet.queue_sel & 1].queue_ready;
    case VREG_INT_STATUS:      return vnet.int_status;
    case VREG_STATUS:          return vnet.status;
    case VREG_CONFIG_GEN:      return 0u;
    default:                   return 0u;
    }
}

static void vnet_store(uint32_t addy, uint32_t val, uint8_t *ram) {
    uint32_t r = addy - VIRTIO_NET_BASE;
    vnet_queue_t *q = &vnet.q[vnet.queue_sel & 1];
    switch (r) {
    case VREG_DEV_FEAT_SEL:   vnet.dev_feat_sel = val; break;
    case VREG_DRV_FEAT_SEL:   vnet.drv_feat_sel = val; break;
    case VREG_DRIVER_FEATURES: break;
    case VREG_QUEUE_SEL:      vnet.queue_sel = val; break;
    case VREG_QUEUE_NUM:      q->queue_num = val < VQUEUE_SIZE ? val : VQUEUE_SIZE; break;
    case VREG_QUEUE_READY:    q->queue_ready = val; break;
    case VREG_QUEUE_NOTIFY:
        if (val == 1) vnet_process_tx(ram);
        vnet_pump_rx(ram);
        break;
    case VREG_INT_ACK:
        vnet.int_status &= ~val;
        plic_set_pending(PLIC_SRC_NET, vnet.int_status != 0);
        break;
    case VREG_STATUS:
        vnet.status = val;
        if (val == 0) {
            for (int i = 0; i < 2; i++) { vnet.q[i].queue_ready = 0; vnet.q[i].last_avail_idx = 0; }
            vnet.int_status = 0;
            plic_set_pending(PLIC_SRC_NET, false);
        }
        break;
    case VREG_QUEUE_DESC_LO:   q->queue_desc_lo   = val; break;
    case VREG_QUEUE_DESC_HI:   break;
    case VREG_QUEUE_DRIVER_LO: q->queue_driver_lo = val; break;
    case VREG_QUEUE_DRIVER_HI: break;
    case VREG_QUEUE_DEVICE_LO: q->queue_device_lo = val; break;
    case VREG_QUEUE_DEVICE_HI: break;
    default: break;
    }
}

#endif /* PLAT_HAS_NET */
