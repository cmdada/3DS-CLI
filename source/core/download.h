#ifndef CORE_DOWNLOAD_H
#define CORE_DOWNLOAD_H

/* Fetching the guest Image over HTTPS.
 *
 * One transfer, in one situation: the card has no Image at all, which leaves
 * the app with nothing to boot and the user with a five-line error and a trip
 * to a PC. Everything else about the install stays manual.
 *
 * Built only where PLAT_HAS_DOWNLOAD says devkitPro has a curl portlib for the
 * console (3DS, Wii U, Switch). libogc has no TLS at all, so the Wii and
 * GameCube never compile any of this - see each plat_cfg.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>   /* ftruncate, for preallocating the download */

#include "plat.h"

/* `latest/download` is a redirect GitHub maintains, so no release tag is baked
   into the binary. It does mean an old app can pull a newer kernel than it was
   tested against - issue #5 was exactly that mismatch - but this is only ever
   reached with no Image on the card, where any kernel beats none, and a card
   that already has one is never touched. */
#define DL_IMAGE_URL \
  "https://github.com/cmdada/3DS-CLI/releases/latest/download/Image"

/* Called as bytes arrive. `total` is 0 until the response headers give one -
   which they may never do. Returning false aborts the transfer. */
typedef bool (*dl_progress_fn)(uint64_t got, uint64_t total, void *ctx);

#ifdef PLAT_HAS_DOWNLOAD

#include <curl/curl.h>

typedef struct {
  FILE          *out;
  dl_progress_fn cb;
  void          *ctx;
  bool           sized;
} dl_sink;

static size_t dl_on_data(char *p, size_t sz, size_t n, void *ud) {
  dl_sink *s = (dl_sink *)ud;
  return fwrite(p, 1, sz * n, s->out);
}

static int dl_on_progress(void *ud, curl_off_t total, curl_off_t got,
                          curl_off_t ul_total, curl_off_t ul_got) {
  dl_sink *s = (dl_sink *)ud;
  (void)ul_total; (void)ul_got;

  /* Set the final length the moment Content-Length says what it is, rather
     than growing the file 16KB at a time for ~55MB. Extending a file on FAT
     walks and extends its cluster chain, which gets more expensive the longer
     the chain already is - the first-boot rootfs extraction hit exactly this
     and ran at a fraction of the card's write speed until it preallocated.
     Best-effort: if the filesystem declines, the writes still work. */
  if (!s->sized && total > 0) {
    s->sized = true;
    ftruncate(fileno(s->out), (off_t)total);
  }

  if (!s->cb) return 0;
  return s->cb((uint64_t)got, total > 0 ? (uint64_t)total : 0, s->ctx) ? 0 : 1;
}

/* Downloads `url` to `dest`. False on any failure, with no partial file left
   behind under `dest`. curl_easy_perform blocks for the whole transfer, so the
   caller's progress callback is the app's only chance to repaint, notice a
   cancel, or answer the OS - see DownloadProgress in machine.c. */
static bool dl_fetch(const char *url, const char *dest, dl_progress_fn cb, void *ctx) {
  /* The .part-then-rename dance the rootfs extraction uses, for the same
     reason: a transfer that dies halfway - the WiFi drops, the user cancels,
     the battery goes - must not leave a truncated file under the name every
     later launch will try to boot. */
  char part[256];
  snprintf(part, sizeof(part), "%s.part", dest);

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
  CURL *c = curl_easy_init();
  if (!c) { curl_global_cleanup(); return false; }

  dl_sink sink = { fopen(part, "wb"), cb, ctx, false };
  if (!sink.out) { curl_easy_cleanup(c); curl_global_cleanup(); return false; }
  /* Newlib's default is a kilobyte, which turns each of curl's chunks into
     several small writes to the card. */
  setvbuf(sink.out, NULL, _IOFBF, 64 * 1024);

  curl_easy_setopt(c, CURLOPT_URL, url);
  /* The release URL redirects to the CDN host the asset really lives on, and
     that host redirects again. */
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  /* Not one of these consoles has a CA store that reaches GitHub's current
     roots - Sectigo E46 and ISRG Root YR both postdate every machine here -
     and none has anywhere to keep an updated bundle. So the chain is not
     checked, and what the caller relies on instead is the RISC-V header check
     on the finished file. */
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "3ds-cli (" PLAT_NAME ")");
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_on_progress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
  /* A console carried out of WiFi range otherwise sits in recv() until the TCP
     stack gives up on its own, with the whole UI frozen behind it. */
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);

  CURLcode rc = curl_easy_perform(c);
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(c);
  curl_global_cleanup();

  /* fclose, not fflush: on these consoles the last few hundred KB are still in
     the FILE buffer when perform() returns, and a write that fails there fails
     silently unless the close is checked. */
  bool ok = rc == CURLE_OK && status == 200;
  if (fclose(sink.out) != 0) ok = false;
  if (!ok) { remove(part); return false; }

  /* rename() onto an existing name fails on FAT. */
  remove(dest);
  if (rename(part, dest) != 0) { remove(part); return false; }
  return true;
}

#endif /* PLAT_HAS_DOWNLOAD */

#endif /* CORE_DOWNLOAD_H */
