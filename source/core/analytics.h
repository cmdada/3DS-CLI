#ifndef CORE_ANALYTICS_H
#define CORE_ANALYTICS_H

/* One ping per launch, to a.adabit.org: which console this is, which model,
 * which version, and a random id the app made up about itself the first time
 * it ran.
 *
 * I mostly just wanted this to see if people are actually running it on other
 * consoles :3
 */

#include "plat.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#ifndef APP_VERSION
/* Overridable from the build (-DAPP_VERSION='"v1.2"'); the release job does
   not stamp it yet I should probably get on that */
#define APP_VERSION "dev"
#endif

#ifdef PLAT_HAS_NET

#include "plat_sock.h"

#ifndef AN_HOST
#define AN_HOST      "a.adabit.org"
#endif
#ifndef AN_PORT
#define AN_PORT      80
#endif
// Cloudflare's own resolver, which is also where the endpoint lives

#ifndef AN_DNS_IP
#define AN_DNS_IP    0x01010101u   /* 1.1.1.1, host order */
#endif
#ifndef AN_DNS_PORT
#define AN_DNS_PORT  53
#endif

// The whole exchange, I don't want analytics to break stuff
#define AN_DEADLINE_US (20ull * 1000ull * 1000ull)
/* A dropped UDP query is the common failure on a console that has just
   associated (at least on my dubious dorm wifi)*/
#define AN_DNS_RETRY_US (2ull * 1000ull * 1000ull)

typedef enum {
  AN_OFF = 0,     /* disabled, finished, or given up on */
  AN_DNS,
  AN_CONNECT,
  AN_SEND,
} an_state_t;

typedef enum { AN_R_PENDING = 0, AN_R_OFF, AN_R_SENT, AN_R_FAILED } an_result_t;

static struct {
  an_state_t state;
  int        fd;
  uint64_t   started;
  uint64_t   last_query;
  uint16_t   txid;
  uint32_t   ip;          /* host order, once resolved */
  char       req[320];
  int        req_len;
  int        req_sent;
  an_result_t result;
} an;

/* ------------------------------------------------------------------ bits - */

static void an_stop(an_result_t r) {
  if (an.fd >= 0) { closesocket(an.fd); an.fd = -1; }
  an.state = AN_OFF;
  an.result = r;
}

/* AN_R_PENDING until it settles, then the same answer for the rest of the
   session. */
static an_result_t an_result(void) { return an.result; }

static void an_rand(void *out, size_t len) {
  if (plat_random(out, len)) return;

  /* No CSPRNG here - the Wii and GameCube have none at all, and the 3DS's is
     behind a service that is not always up. The id only has to be unlikely to
     collide with another console's; nothing is authorised by it.
  */
  uint64_t s = plat_wallclock_ms();
  s ^= plat_us() * 0x9e3779b97f4a7c15ull;
  s ^= (uint64_t)(uintptr_t)out << 17;

  uint8_t *p = (uint8_t *)out;
  for (size_t i = 0; i < len; i++) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    p[i] = (uint8_t)(s >> 33);
  }
}

/* plat_model() is the boot banner's line, "<model> (<diagnostics>)" on every
   console which is firmware versions, clock speeds, which trees mounted. Only the
   half before the bracket is a model */
static void an_model(char *out, int cap) {
  const char *m = plat_model();
  const char *br = strstr(m, " (");
  int n = br ? (int)(br - m) : (int)strlen(m);
  if (n > cap - 1) n = cap - 1;
  memcpy(out, m, (size_t)n);
  out[n] = '\0';
}

/* Everything outside the unreserved set, percent-encoded. Model strings are
   the reason: "New 3DS (XL)" has both a space and parentheses in it. */
static int an_urlenc(char *out, int cap, const char *in) {
  static const char *hexd = "0123456789ABCDEF";
  int n = 0;
  for (; *in && n + 4 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
      out[n++] = (char)c;
    } else {
      out[n++] = '%';
      out[n++] = hexd[c >> 4];
      out[n++] = hexd[c & 0x0f];
    }
  }
  out[n] = '\0';
  return n;
}

/* ------------------------------------------------------------------ DNS - */

/* A minimal A query. Writing it out beats calling the console's resolver:
   three of these stacks have no gethostbyname at all, and the two that do
   spell it differently and block. */
static int an_dns_query(uint8_t *buf, int cap, const char *host, uint16_t id) {
  if (cap < 12 + (int)strlen(host) + 2 + 4) return 0;
  int n = 0;
  buf[n++] = (uint8_t)(id >> 8); buf[n++] = (uint8_t)id;
  buf[n++] = 0x01; buf[n++] = 0x00;   /* standard query, recursion desired */
  buf[n++] = 0x00; buf[n++] = 0x01;   /* QDCOUNT */
  buf[n++] = 0x00; buf[n++] = 0x00;   /* ANCOUNT */
  buf[n++] = 0x00; buf[n++] = 0x00;   /* NSCOUNT */
  buf[n++] = 0x00; buf[n++] = 0x00;   /* ARCOUNT */

  const char *p = host;
  while (*p) {
    const char *dot = strchr(p, '.');
    int len = dot ? (int)(dot - p) : (int)strlen(p);
    if (len <= 0 || len > 63) return 0;
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, p, (size_t)len); n += len;
    p = dot ? dot + 1 : p + len;
  }
  buf[n++] = 0x00;
  buf[n++] = 0x00; buf[n++] = 0x01;   /* QTYPE A */
  buf[n++] = 0x00; buf[n++] = 0x01;   /* QCLASS IN */
  return n;
}

/* Steps `off` past one name, compressed or not. A pointer ends the name, so
   there is nothing to follow: only the length matters here. */
static bool an_dns_skip_name(const uint8_t *b, int len, int *off) {
  while (*off < len) {
    uint8_t l = b[*off];
    if ((l & 0xc0) == 0xc0) { *off += 2; return *off <= len; }
    *off += 1;
    if (l == 0) return true;
    *off += l;
  }
  return false;
}

/* The first A record in the answer section, or 0. CNAMEs are stepped over
   a.adabit.org is a Cloudflare proxied record and the chain is resolved for
   us, so the A is always in the same answer. */
static uint32_t an_dns_answer(const uint8_t *b, int len, uint16_t id) {
  if (len < 12) return 0;
  if (((b[0] << 8) | b[1]) != id) return 0;
  if ((b[3] & 0x0f) != 0) return 0;            /* RCODE */
  int qd = (b[4] << 8) | b[5];
  int an_count = (b[6] << 8) | b[7];

  int off = 12;
  for (int i = 0; i < qd; i++) {
    if (!an_dns_skip_name(b, len, &off)) return 0;
    off += 4;
  }

  for (int i = 0; i < an_count && off < len; i++) {
    if (!an_dns_skip_name(b, len, &off)) return 0;
    if (off + 10 > len) return 0;
    int type = (b[off] << 8) | b[off + 1];
    int cls  = (b[off + 2] << 8) | b[off + 3];
    int rdl  = (b[off + 8] << 8) | b[off + 9];
    off += 10;
    if (off + rdl > len) return 0;
    if (type == 1 && cls == 1 && rdl == 4) {
      return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
             ((uint32_t)b[off + 2] << 8) | (uint32_t)b[off + 3];
    }
    off += rdl;
  }
  return 0;
}

static bool an_dns_begin(void) {
  an.fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (an.fd < 0) return false;
  PLAT_SOCK_SET_NONBLOCK(an.fd);
  an.last_query = 0;
  return true;
}

static void an_dns_send(void) {
  uint8_t q[64];
  int n = an_dns_query(q, sizeof(q), AN_HOST, an.txid);
  if (n <= 0) { an_stop(AN_R_FAILED); return; }

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(AN_DNS_PORT);
  sa.sin_addr.s_addr = htonl(AN_DNS_IP);
  sendto(an.fd, (const char *)q, (size_t)n, 0, (struct sockaddr *)&sa, sizeof(sa));
  an.last_query = plat_us();
}

/* ------------------------------------------------------------- the request */

static void an_build_request(const Cfg *cfg) {
  char raw[48], model[96], plat[32], ver[48];
  an_model(raw, sizeof(raw));
  an_urlenc(model, sizeof(model), raw);
  an_urlenc(plat, sizeof(plat), PLAT_SLUG);
  an_urlenc(ver, sizeof(ver), APP_VERSION);

  an.req_len = snprintf(
      an.req, sizeof(an.req),
      "GET /c?p=%s&m=%s&v=%s&i=%s HTTP/1.1\r\n"
      "Host: " AN_HOST "\r\n"
      "User-Agent: 3ds-cli/%s (" PLAT_NAME ")\r\n"
      "Connection: close\r\n"
      "\r\n",
      plat, model, ver, cfg->install_id, APP_VERSION);
  if (an.req_len < 0 || an.req_len >= (int)sizeof(an.req)) an.req_len = 0;
  an.req_sent = 0;
}

static bool an_connect_begin(void) {
  closesocket(an.fd);
  an.fd = socket(AF_INET, SOCK_STREAM, 0);
  if (an.fd < 0) return false;
  PLAT_SOCK_SET_NONBLOCK(an.fd);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(AN_PORT);
  sa.sin_addr.s_addr = htonl(an.ip);
  connect(an.fd, (struct sockaddr *)&sa, sizeof(sa));  /* EINPROGRESS expected */
  return true;
}

/* --------------------------------------------------------------- the API - */

/* Called once the console's socket stack is up, which is the only moment any
   of this is possible. `cfg` is written when the install id is minted, and
   the caller saves it. */
static void an_start(Cfg *cfg, bool *cfg_changed) {
  memset(&an, 0, sizeof(an));
  an.fd = -1;

  if (!cfg->analytics) { an.result = AN_R_OFF; return; }

  if (strlen(cfg->install_id) != 32) {
    uint8_t r[16];
    an_rand(r, sizeof(r));
    for (int i = 0; i < 16; i++) snprintf(cfg->install_id + i * 2, 3, "%02x", r[i]);
    cfg->install_id[32] = '\0';
    if (cfg_changed) *cfg_changed = true;
  }

  an_build_request(cfg);
  if (an.req_len <= 0) { an.result = AN_R_FAILED; return; }

  uint16_t id;
  an_rand(&id, sizeof(id));
  an.txid = id ? id : 1;
  an.started = plat_us();

  if (!an_dns_begin()) { an.result = AN_R_FAILED; return; }
  an.state = AN_DNS;
}

/* One step, from the main loop. Returns immediately once the ping is done or
   has been given up on, which is the usual case for all but the first second
   or so of a session. */
static void an_poll(void) {
  if (an.state == AN_OFF) return;

  if (plat_us() - an.started > AN_DEADLINE_US) { an_stop(AN_R_FAILED); return; }

  switch (an.state) {
    case AN_DNS: {
      if (an.last_query == 0 || plat_us() - an.last_query > AN_DNS_RETRY_US) an_dns_send();

      fd_set rfds; FD_ZERO(&rfds); FD_SET(an.fd, &rfds);
      struct timeval tv = {0, 0};
      if (select(an.fd + 1, &rfds, NULL, NULL, &tv) <= 0 || !FD_ISSET(an.fd, &rfds)) return;

      uint8_t buf[512];
      int n = recv(an.fd, (char *)buf, sizeof(buf), 0);
      if (n <= 0) return;

      uint32_t ip = an_dns_answer(buf, n, an.txid);
      if (!ip) return;            /* someone else's reply, or no A record yet */

      an.ip = ip;
      if (!an_connect_begin()) { an_stop(AN_R_FAILED); return; }
      an.state = AN_CONNECT;
      return;
    }

    case AN_CONNECT: {
      int err = 0; socklen_t el = sizeof(err);
      getsockopt(an.fd, SOL_SOCKET, SO_ERROR, &err, &el);
      if (err != 0) { an_stop(AN_R_FAILED); return; }

      fd_set wfds; FD_ZERO(&wfds); FD_SET(an.fd, &wfds);
      struct timeval tv = {0, 0};
      if (select(an.fd + 1, NULL, &wfds, NULL, &tv) > 0 && FD_ISSET(an.fd, &wfds)) {
        an.state = AN_SEND;
      }
      return;
    }

    case AN_SEND: {
      fd_set wfds; FD_ZERO(&wfds); FD_SET(an.fd, &wfds);
      struct timeval tv = {0, 0};
      if (select(an.fd + 1, NULL, &wfds, NULL, &tv) <= 0 || !FD_ISSET(an.fd, &wfds)) return;

      int n = send(an.fd, an.req + an.req_sent, (size_t)(an.req_len - an.req_sent), 0);
      if (n <= 0) { an_stop(AN_R_FAILED); return; }
      an.req_sent += n;
      // The reply is never read. Nothing here acts on it
      if (an.req_sent >= an.req_len) an_stop(AN_R_SENT);
      return;
    }

    default:
      an_stop(AN_R_FAILED);
      return;
  }
}

#else  /* !PLAT_HAS_NET */

typedef enum { AN_R_PENDING = 0, AN_R_OFF, AN_R_SENT, AN_R_FAILED } an_result_t;

static inline void an_start(Cfg *cfg, bool *cfg_changed) { (void)cfg; (void)cfg_changed; }
static inline void an_poll(void) {}
static inline an_result_t an_result(void) { return AN_R_OFF; }

#endif /* PLAT_HAS_NET */

#endif /* CORE_ANALYTICS_H */
