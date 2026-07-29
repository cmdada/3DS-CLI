#include <3ds.h>
#include <zlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "draw.h"
#include "terminal.h"

TermState term_state;

static void term_printf(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  
  char *s = buf;
  while (*s) {
    term_write_char(&term_state, *s++);
  }
}

// Special key codes
#define K_SHIFT 1
#define K_SYM   2
#define K_ABC   3
#define K_SYM2  4
#define K_CTRL  5
#define K_TAB   '\t'
#define K_ESC   27
#define K_ENTER '\n'
#define K_BKSP  '\b'
#define K_SPACE ' '

const char keymap_lower[5][10] = {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l','\''},
    {K_SHIFT,'z','x','c','v','b','n','m',',','.'},
    {K_TAB,K_ESC,K_SYM,K_SPACE,K_SPACE,K_SPACE,K_SPACE,K_CTRL,K_ENTER,K_BKSP}};
const char keymap_upper[5][10] = {
    {'!','@','#','$','%','^','&','*','(',')'},
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L','"'},
    {K_SHIFT,'Z','X','C','V','B','N','M',';',':'},
    {K_TAB,K_ESC,K_SYM,K_SPACE,K_SPACE,K_SPACE,K_SPACE,K_CTRL,K_ENTER,K_BKSP}};
const char keymap_sym1[5][10] = {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'+','-','*','/','=','|','\\','`','~','_'},
    {'@','#','$','%','^','&','(',')','[',']'},
    {K_SYM2,'{','}','<','>','?','!',':',';','"'},
    {K_TAB,K_ESC,K_ABC,K_SPACE,K_SPACE,K_SPACE,K_SPACE,K_CTRL,K_ENTER,K_BKSP}};
const char keymap_sym2[5][10] = {
    {'`','~','|','\\','_','-','+','=','.',','},
    {'[',']','{','}','<','>','(',')','/','?'},
    {'!','@','#','$','%','^','&','*','\'','"'},
    {K_ABC,':',';','-','_','+','=','\\','|','~'},
    {K_TAB,K_ESC,K_ABC,K_SPACE,K_SPACE,K_SPACE,K_SPACE,K_CTRL,K_ENTER,K_BKSP}};

int kb_layer = 0;
bool ctrl_active = false;
static int pressed_key_r = -1;
static int pressed_key_c = -1;

const char (*get_keymap(void))[10] {
  switch(kb_layer) {
    case 1: return keymap_upper; case 2: return keymap_sym1;
    case 3: return keymap_sym2; default: return keymap_lower;
  }
}
bool is_modifier(char k) {
  unsigned char u=(unsigned char)k;
  return u==K_SHIFT||u==K_SYM||u==K_ABC||u==K_SYM2||u==K_CTRL;
}

// Layout constants
#define KB_Y0    38   // keyboard top pixel
#define KEY_W    30   // key width
#define KEY_H    36   // key height
#define GAP      2    // gap between keys
#define COLS     10
// Row X start: center 10 keys in 320px
// 10*30 + 9*2 = 318, offset = 1
#define KB_X0    1

static int key_x(int c) { return KB_X0 + c * (KEY_W + GAP); }
static int key_y(int r) { return KB_Y0 + r * (KEY_H + GAP); }

static bool key_is_pressed(int r, int c) {
  return pressed_key_r == r && pressed_key_c == c;
}

static int normalize_key_col(int r, int c) {
  if (r == 4 && c >= 3 && c <= 6) return 3;
  return c;
}

static void draw_key_body(int x, int y, int w, int h,
                          u8 kr, u8 kg, u8 kb_c, u8 hr, u8 hg, u8 hb_c,
                          u8 sr, u8 sg, u8 sb, bool pressed) {
  if (pressed) {
    fb_key3d(x + 1, y + 2, w - 2, h - 2,
             sr, sg, sb, kr, kg, kb_c, hr, hg, hb_c);
  } else {
    fb_key3d(x, y, w, h, kr, kg, kb_c, hr, hg, hb_c, sr, sg, sb);
  }
}

// Draw a single key at grid position (r, c)
static void draw_key_at(int r, int c, char key, const char *label, int is_wide,
                        u8 kr, u8 kg, u8 kb_c, u8 hr, u8 hg, u8 hb_c,
                        u8 sr, u8 sg, u8 sb,
                        u8 tr, u8 tg, u8 tb_c, int tscale) {
  int x = key_x(c);
  int y = key_y(r);
  int w = is_wide ? (KEY_W * is_wide + GAP * (is_wide - 1)) : KEY_W;
  bool pressed = key_is_pressed(r, c);
  int text_y = y + (pressed ? 2 : 0);
  // 3D key
  draw_key_body(x, y, w, KEY_H, kr, kg, kb_c, hr, hg, hb_c, sr, sg, sb, pressed);
  // Text
  if (label) {
    int th = 8 * tscale;
    fb_string_centered(x, text_y + (KEY_H - th) / 2 - 1, w, label, tr, tg, tb_c, tscale);
  } else if (key >= 32 && key <= 126) {
    char buf[2] = {key, 0};
    int th = 8 * tscale;
    fb_string_centered(x, text_y + (KEY_H - th) / 2 - 1, w, buf, tr, tg, tb_c, tscale);
  }
}

void draw_keyboard() {
  fb_init();
  // Background
  fb_clear(CLR_BG_R, CLR_BG_G, CLR_BG_B);

  // Status bar background
  fb_fill(0, 0, 320, 38, CLR_SB_R, CLR_SB_G, CLR_SB_B);
  fb_string(4, 4, "3ds-cli", CLR_TXT_R, CLR_TXT_G, CLR_TXT_B, 1);
  fb_string(68, 4, "adabit.org", CLR_TXTD_R, CLR_TXTD_G, CLR_TXTD_B, 1);

  // Layer badge
  const char *ltag;
  u8 ltr, ltg, ltb;
  switch(kb_layer) {
    case 0: ltag="abc"; ltr=100; ltg=200; ltb=130; break;
    case 1: ltag="ABC"; ltr=100; ltg=200; ltb=130; break;
    case 2: ltag="?#1"; ltr=255; ltg=200; ltb=100; break;
    case 3: ltag="#+="; ltr=255; ltg=200; ltb=100; break;
    default: ltag=""; ltr=ltg=ltb=200; break;
  }
  fb_rrect(270, 2, 46, 13, 45, 45, 52);
  fb_string_centered(270, 4, 46, ltag, ltr, ltg, ltb, 1);

  // Info line
  if (ctrl_active) {
    fb_rrect(4, 18, 50, 14, CLR_CTLA_R, CLR_CTLA_G, CLR_CTLA_B);
    fb_string(8, 21, "CTRL", 255, 255, 255, 1);
    fb_string(60, 21, "tap a key...", CLR_TXTD_R, CLR_TXTD_G, CLR_TXTD_B, 1);
  } else {
  }

  // Separator line
  fb_hline(0, 40, 320, 45, 45, 55);

  const char (*map)[10] = get_keymap();

  // Rows 0-3: regular keys
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 10; c++) {
      char key = map[r][c];
      unsigned char uk = (unsigned char)key;

      if (is_modifier(key)) {
        // Modifier keys
        const char *lbl = NULL;
        u8 mr=CLR_MOD_R, mg=CLR_MOD_G, mb=CLR_MOD_B;
        u8 mhr=CLR_MODHI_R, mhg=CLR_MODHI_G, mhb=CLR_MODHI_B;
        u8 msr=CLR_MODSH_R, msg=CLR_MODSH_G, msb=CLR_MODSH_B;
        u8 mtr=CLR_TXT_R, mtg=CLR_TXT_G, mtb=CLR_TXT_B;

        if (uk == K_SHIFT) {
          lbl = "SHF";
          if (kb_layer == 1) { mr=CLR_ACTV_R; mg=CLR_ACTV_G; mb=CLR_ACTV_B;
                               mhr=mr+15; mhg=mg+15; mhb=mb+15;
                               msr=mr-10; msg=mg-10; msb=mb-10; }
        } else if (uk == K_SYM) {
          lbl = "?#1"; mr=CLR_SYM_R; mg=CLR_SYM_G; mb=CLR_SYM_B;
          mhr=mr+12; mhg=mg+12; mhb=mb+12; msr=mr-10; msg=mg-10; msb=mb-10;
        } else if (uk == K_ABC) {
          lbl = "ABC"; mtr=130; mtg=180; mtb=255;
        } else if (uk == K_SYM2) {
          lbl = "#+="; mr=CLR_SYM_R; mg=CLR_SYM_G; mb=CLR_SYM_B;
          mhr=mr+12; mhg=mg+12; mhb=mb+12; msr=mr-10; msg=mg-10; msb=mb-10;
        } else if (uk == K_CTRL) {
          lbl = "CTL";
          if (ctrl_active) { mr=CLR_CTLA_R; mg=CLR_CTLA_G; mb=CLR_CTLA_B;
                             mhr=mr+15; mhg=mg+10; mhb=mb+10;
                             msr=mr-12; msg=mg-8; msb=mb-8; mtr=255; mtg=220; mtb=220; }
        }
        draw_key_at(r, c, 0, lbl, 0, mr, mg, mb, mhr, mhg, mhb, msr, msg, msb, mtr, mtg, mtb, 1);
      } else {
        // Normal character key
        u8 kr, kg, kbc, hr2, hg2, hb2, sr2, sg2, sb2, tr2, tg2, tb2;
        int sc = 2;
        if (r == 0) {
          kr=CLR_NUM_R; kg=CLR_NUM_G; kbc=CLR_NUM_B;
          tr2=CLR_TXTN_R; tg2=CLR_TXTN_G; tb2=CLR_TXTN_B;
        } else {
          kr=CLR_KEY_R; kg=CLR_KEY_G; kbc=CLR_KEY_B;
          tr2=CLR_TXT_R; tg2=CLR_TXT_G; tb2=CLR_TXT_B;
        }
        hr2=CLR_KEYHI_R; hg2=CLR_KEYHI_G; hb2=CLR_KEYHI_B;
        sr2=CLR_KEYSH_R; sg2=CLR_KEYSH_G; sb2=CLR_KEYSH_B;
        // Symbols on sym layers render at 2x too
        draw_key_at(r, c, key, NULL, 0, kr, kg, kbc, hr2, hg2, hb2, sr2, sg2, sb2, tr2, tg2, tb2, sc);
      }
    }
  }

  // Row 4: bottom bar - TAB ESC SYM [---SPACE---] CTL ENT DEL
  int r4 = 4;
  // TAB
  draw_key_at(r4, 0, 0, "TAB", 0,
    CLR_MOD_R,CLR_MOD_G,CLR_MOD_B, CLR_MODHI_R,CLR_MODHI_G,CLR_MODHI_B,
    CLR_MODSH_R,CLR_MODSH_G,CLR_MODSH_B, 100,200,220, 1);
  // ESC
  draw_key_at(r4, 1, 0, "ESC", 0,
    CLR_MOD_R,CLR_MOD_G,CLR_MOD_B, CLR_MODHI_R,CLR_MODHI_G,CLR_MODHI_B,
    CLR_MODSH_R,CLR_MODSH_G,CLR_MODSH_B, 200,140,220, 1);
  // SYM/ABC
  {
    const char *sl; u8 sr3,sg3,sb3;
    if (kb_layer >= 2) { sl="ABC"; sr3=130; sg3=180; sb3=255; }
    else { sl="?#1"; sr3=255; sg3=200; sb3=100; }
    draw_key_at(r4, 2, 0, sl, 0,
      CLR_SYM_R,CLR_SYM_G,CLR_SYM_B, CLR_SYM_R+12,CLR_SYM_G+12,CLR_SYM_B+12,
      CLR_SYM_R-10,CLR_SYM_G-10,CLR_SYM_B-10, sr3,sg3,sb3, 1);
  }
  // SPACE (cols 3-6 = 4 keys wide)
  {
    int sx = key_x(3), sy = key_y(r4);
    int sw = KEY_W * 4 + GAP * 3;
    bool pressed = key_is_pressed(r4, 3);
    draw_key_body(sx, sy, sw, KEY_H, CLR_SPC_R,CLR_SPC_G,CLR_SPC_B,
                  CLR_SPC_R+14,CLR_SPC_G+14,CLR_SPC_B+14,
                  CLR_SPC_R-10,CLR_SPC_G-10,CLR_SPC_B-10, pressed);
    // Small dot/line in center to indicate space
    fb_fill(sx + sw/2 - 15, sy + KEY_H/2 + (pressed ? 2 : 0), 30, 1, CLR_TXTD_R, CLR_TXTD_G, CLR_TXTD_B);
  }
  // CTL
  {
    u8 cr, cg, cb;
    if (ctrl_active) { cr=CLR_CTLA_R; cg=CLR_CTLA_G; cb=CLR_CTLA_B; }
    else { cr=CLR_MOD_R; cg=CLR_MOD_G; cb=CLR_MOD_B; }
    draw_key_at(r4, 7, 0, "CTL", 0,
      cr,cg,cb, cr+15,cg+10,cb+10, cr-10,cg-8,cb-8,
      ctrl_active?255:CLR_TXT_R, ctrl_active?220:CLR_TXT_G, ctrl_active?220:CLR_TXT_B, 1);
  }
  // ENTER
  draw_key_at(r4, 8, 0, "ENT", 0,
    CLR_ENT_R,CLR_ENT_G,CLR_ENT_B, CLR_ENT_R+18,CLR_ENT_G+18,CLR_ENT_B+18,
    CLR_ENT_R-10,CLR_ENT_G-10,CLR_ENT_B-10, 200,255,210, 1);
  // BKSP
  draw_key_at(r4, 9, 0, "DEL", 0,
    CLR_DEL_R,CLR_DEL_G,CLR_DEL_B, CLR_DEL_R+15,CLR_DEL_G+10,CLR_DEL_B+10,
    CLR_DEL_R-12,CLR_DEL_G-8,CLR_DEL_B-8, 255,200,200, 1);

  gfxFlushBuffers();
}

// ---------------------------------------------------------
// mini-rv32ima Integration
// ---------------------------------------------------------
#include "default64mbdtc.h"

uint32_t ram_amt = 64 * 1024 * 1024;
uint8_t *ram_image = 0;
struct MiniRV32IMAState *core;

#define TICKS_PER_US 268
#define EMU_RUN_BUDGET_US 50000
#define EMU_STEP_CHUNK 200000
#define TOP_REFRESH_INTERVAL_US 33000

char rx_buf[256];
int rx_head = 0, rx_tail = 0;

static int IsKBHit() { return rx_head != rx_tail; }
static int ReadKBByte() {
  if (rx_head == rx_tail) return -1;
  char c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % 256;
  return c;
}
static void rx_push(char c) {
  int nh = (rx_head + 1) % 256;
  if (nh != rx_tail) { rx_buf[rx_head] = c; rx_head = nh; }
}
static void rx_push_str(const char *s) { while (*s) rx_push(*s++); }

static uint32_t uart_byte_count = 0;
static FILE *uart_log_file = NULL; // sdmc:/3ds-cli-console.log mirror of guest console output
static void WriteUARTByte(char c) {
  uart_byte_count++;
  term_state.auto_track = true;
  term_write_char(&term_state, c);
  if (uart_log_file) {
    fputc(c, uart_log_file);
    if (c == '\n') fflush(uart_log_file);
  }
}

static bool TimeSinceUs(uint64_t last_tick, uint64_t interval_us) {
  return svcGetSystemTick() - last_tick >= interval_us * TICKS_PER_US;
}

static void PresentTopScreen(uint64_t *last_present_tick) {
  u8 *top_fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
  term_draw(&term_state, top_fb);
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
  *last_present_tick = svcGetSystemTick();
  term_state.dirty = false;
}

static FILE *OpenDiskFile(const char **opened_path) {
  static const char *paths[] = { "sdmc:/rootfs.ext2", "rootfs.ext2" };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    FILE *f = fopen(paths[i], "r+b");
    if (f) {
      if (opened_path) *opened_path = paths[i];
      return f;
    }
  }
  if (opened_path) *opened_path = paths[0];
  return NULL;
}

/* The Image file may be a combined bundle produced by tools/mkimage.py:
   [kernel][gzipped rootfs][u32 gz_len][u32 raw_len]["3DSCLIRF"]. Detect
   the 16-byte trailer and return the kernel-only length (or the full file
   length for a plain kernel Image, which stays fully supported). */
#define IMAGE_TRAILER_MAGIC "3DSCLIRF"
static long ImageKernelLen(FILE *f, long flen, uint32_t *gz_len_out, uint32_t *raw_len_out) {
  uint8_t tr[16];
  if (flen <= 16) return flen;
  fseek(f, flen - 16, SEEK_SET);
  if (fread(tr, sizeof(tr), 1, f) != 1) { fseek(f, 0, SEEK_SET); return flen; }
  fseek(f, 0, SEEK_SET);
  if (memcmp(tr + 8, IMAGE_TRAILER_MAGIC, 8) != 0) return flen;
  uint32_t gz_len, raw_len;
  memcpy(&gz_len, tr + 0, 4);
  memcpy(&raw_len, tr + 4, 4);
  if ((long)gz_len + 16 >= flen) return flen; /* implausible: treat as plain */
  if (gz_len_out) *gz_len_out = gz_len;
  if (raw_len_out) *raw_len_out = raw_len;
  return flen - 16 - (long)gz_len;
}

/* One-time first-boot extraction of the embedded rootfs to the SD card,
   where it lives as the writable, persistent filesystem from then on. */
static bool ExtractEmbeddedRootfs(FILE *f, long gz_off, uint32_t gz_len, uint32_t raw_len,
                                  uint64_t *present_tick) {
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, 15 + 16 /* gzip wrapper */) != Z_OK) return false;

  FILE *out = fopen("sdmc:/rootfs.ext2", "wb");
  if (!out) { inflateEnd(&zs); return false; }

  static uint8_t inbuf[64 * 1024], outbuf[256 * 1024];
  uint32_t in_left = gz_len, done = 0, last_mb = 0;
  bool ok = true;
  fseek(f, gz_off, SEEK_SET);
  term_printf("First boot: extracting rootfs.ext2 (%luMB) to SD...\n",
              (unsigned long)(raw_len >> 20));
  PresentTopScreen(present_tick);

  int zret = Z_OK;
  while (zret != Z_STREAM_END) {
    if (zs.avail_in == 0 && in_left > 0) {
      uint32_t n = in_left < sizeof(inbuf) ? in_left : sizeof(inbuf);
      if (fread(inbuf, n, 1, f) != 1) { ok = false; break; }
      zs.next_in = inbuf; zs.avail_in = n; in_left -= n;
    }
    zs.next_out = outbuf; zs.avail_out = sizeof(outbuf);
    zret = inflate(&zs, Z_NO_FLUSH);
    if (zret != Z_OK && zret != Z_STREAM_END) { ok = false; break; }
    uint32_t produced = sizeof(outbuf) - zs.avail_out;
    if (produced && fwrite(outbuf, produced, 1, out) != 1) { ok = false; break; }
    done += produced;
    if ((done >> 20) >= last_mb + 16) {
      last_mb = done >> 20;
      term_printf("  ... %luMB / %luMB\n", (unsigned long)last_mb, (unsigned long)(raw_len >> 20));
      PresentTopScreen(present_tick);
    }
  }
  inflateEnd(&zs);
  fclose(out);
  fseek(f, 0, SEEK_SET);
  if (!ok || done != raw_len) {
    term_printf("Extraction failed, removing partial rootfs.\n");
    remove("sdmc:/rootfs.ext2");
    return false;
  }
  term_printf("Rootfs extracted.\n");
  PresentTopScreen(present_tick);
  return true;
}

/* Backing store for the guest's swap device (/dev/vdb). Guest RAM is only
   whatever the 3DS heap could spare (8-64MB, see the malloc loop in main),
   so this is what keeps anything nontrivial from being OOM-killed.
   Created fresh each launch and removed on exit, so it never becomes a
   stale file the user has to know about.

   Note this is deliberately *not* zero-filled: the file is created by
   seeking to the last byte and writing there, which costs nothing beyond
   allocating the space. Swap only ever reads back pages it wrote itself,
   so whatever the filesystem leaves in the gap is never observed. Some SD
   filesystems don't extend a file that way, so the result is verified and
   falls back to an explicit (slow, one-time) zero-fill. */
#define SWAP_PATH        "sdmc:/swap.img"
#define SWAP_SIZE_BYTES  (64u * 1024u * 1024u)

static FILE *CreateSwapFile(uint64_t *present_tick) {
  FILE *f = fopen(SWAP_PATH, "w+b");
  if (!f) return NULL;

  if (fseek(f, (long)SWAP_SIZE_BYTES - 1, SEEK_SET) == 0 && fputc(0, f) != EOF) {
    fflush(f);
    fseek(f, 0, SEEK_END);
    if ((uint64_t)ftell(f) == SWAP_SIZE_BYTES) { rewind(f); return f; }
  }

  /* Sparse extend didn't take: write the whole thing out, with progress,
     the same way the first-boot rootfs extraction reports itself. Small
     stack buffer rather than a static one - this path is rare, SD
     throughput dominates the loop anyway, and any permanent buffer here
     comes straight out of the heap the guest's RAM is carved from. */
  uint8_t zeros[4096];
  memset(zeros, 0, sizeof(zeros));
  rewind(f);
  term_printf("Creating swap file (%luMB)...\n", (unsigned long)(SWAP_SIZE_BYTES >> 20));
  PresentTopScreen(present_tick);
  uint32_t written = 0, last_mb = 0;
  while (written < SWAP_SIZE_BYTES) {
    uint32_t n = SWAP_SIZE_BYTES - written;
    if (n > sizeof(zeros)) n = sizeof(zeros);
    if (fwrite(zeros, n, 1, f) != 1) {
      fclose(f);
      remove(SWAP_PATH);
      return NULL;
    }
    written += n;
    if ((written >> 20) >= last_mb + 16) {
      last_mb = written >> 20;
      term_printf("  ... %luMB / %luMB\n", (unsigned long)last_mb,
                  (unsigned long)(SWAP_SIZE_BYTES >> 20));
      PresentTopScreen(present_tick);
    }
  }
  fflush(f);
  rewind(f);
  return f;
}

static FILE *dbg_log_file = NULL;

/* File-scope so every exit path (including the `goto wait_exit`s that fire
   before it's even created) can clean it up unconditionally. */
static FILE *swap_file = NULL;

static void CloseSwapFile(void) {
  if (swap_file) { fclose(swap_file); swap_file = NULL; }
  remove(SWAP_PATH);
}

static uint32_t HandleException(uint32_t ir, uint32_t retval);
static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy);
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value);
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno);

#define MINIRV32WARN(x...) printf(x);
#define MINIRV32_DECORATE static
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval) { if (retval > 0) retval = HandleException(ir, retval); }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value) HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value) value = HandleOtherCSRRead(image, csrno);

#include "mini-rv32ima.h"
#include "plic.h"
#include "virtio_blk.h"
#include "virtio_rng.h"
#include "virtio_net.h"
#include "virtio_9p.h"
#include "virtio_input.h"
#include "rtc_goldfish.h"

static int sbi_shutdown_requested = 0;

// This core boots straight into S-mode with no real M-mode firmware ever
// running (see vendor/mini-rv32ima-mmu's file header for why), so this
// hook has to act as that firmware itself for the two things a NOMMU-less
// MMU'd Linux kernel expects M-mode to service: SBI ecalls, and forwarding
// the real machine timer interrupt to the guest as a supervisor timer
// interrupt (there's no Sstc-style hardware STIP wire to do that for us).
static void HandleSBICall(void) {
  uint32_t eid = core->regs[17]; // a7
  uint32_t fid = core->regs[16]; // a6
  uint32_t a0 = core->regs[10];
  uint32_t a1 = core->regs[11];
  uint32_t ret_err = 0, ret_val = 0;

  switch (eid) {
    case 0x10: // Base extension
      switch (fid) {
        case 0: ret_val = 0x1000000; break; // get_spec_version: v1.0
        case 1: ret_val = 0xff; break;       // get_impl_id: unregistered/"other"
        case 2: ret_val = 1; break;          // get_impl_version
        case 3: // probe_extension
          switch (a0) {
            case 0x10: case 0x54494D45: case 0x4442434E: case 0x53525354:
            case 0: case 1: case 8:
              ret_val = 1; break;
            default: ret_val = 0; break;
          }
          break;
        case 4: case 5: case 6: ret_val = 0; break; // mvendorid/marchid/mimpid
        default: ret_err = (uint32_t)-2; break; // SBI_ERR_NOT_SUPPORTED
      }
      break;
    case 0x54494D45: // TIME extension
      if (fid == 0) { // set_timer(a0=stime_lo, a1=stime_hi)
        core->timermatchl = a0;
        core->timermatchh = a1;
        core->mip &= ~(1u << 5); // Clear STIP: we're (re)scheduling.
      } else ret_err = (uint32_t)-2;
      break;
    case 0x4442434E: // DBCN debug console (preferred over the legacy console calls below)
      if (fid == 2) { // console_write_byte(a0=byte)
        WriteUARTByte((char)a0);
      } else if (fid == 0) { // console_write(a0=num_bytes, a1=addr_lo)
        uint32_t ofs = a1 - MINIRV32_RAM_IMAGE_OFFSET;
        for (uint32_t i = 0; i < a0 && ofs + i < ram_amt; i++) WriteUARTByte((char)ram_image[ofs + i]);
        ret_val = a0;
      } else if (fid == 1) { // console_read - no input support, report 0 bytes read.
        ret_val = 0;
      } else ret_err = (uint32_t)-2;
      break;
    case 1: // Legacy console_putchar(a0=char)
      WriteUARTByte((char)a0);
      break;
    case 2: // Legacy console_getchar
      ret_val = IsKBHit() ? (uint32_t)(uint8_t)ReadKBByte() : (uint32_t)-1;
      break;
    case 0x53525354: // SRST system reset
      sbi_shutdown_requested = 1;
      break;
    default:
      ret_err = (uint32_t)-2;
      break;
  }

  // Legacy extensions (0-8) return their value directly in a0; everything
  // from the base extension onward uses the (a0=error, a1=value) convention.
  if (eid <= 8) core->regs[10] = ret_val;
  else { core->regs[10] = ret_err; core->regs[11] = ret_val; }
}

static uint32_t HandleException(uint32_t ir, uint32_t retval) {
  if (retval == 0x80000007) {
    // Real machine timer interrupt: forward it to the guest as a
    // supervisor timer interrupt instead of taking a (nonexistent)
    // M-mode trap.
    core->mip |= (1u << 5); // STIP
    return 0;
  }
  if (retval == (9 + 1)) { // ECALL from S-mode == SBI call, never delegated.
    HandleSBICall();
    return 0;
  }
  return retval;
}
static uint32_t HandleControlStore(uint32_t addy, uint32_t val) {
  vblk_dev_t *bd;
  if (addy == 0x10000000) WriteUARTByte((char)val);
  else if (addy == 0x11004004) core->timermatchh = val;
  else if (addy == 0x11004000) core->timermatchl = val;
  else if (addy == 0x11100000) { core->pc += 4; return val; }
  else if ((bd = vblk_for_addr(addy)) != NULL)
    vblk_store(bd, addy, val, ram_image);
  else if (addy >= VIRTIO_NET_BASE && addy < VIRTIO_NET_BASE + VIRTIO_NET_SIZE)
    vnet_store(addy, val, ram_image);
  else if (addy >= VIRTIO_RNG_BASE && addy < VIRTIO_RNG_BASE + VIRTIO_RNG_SIZE)
    vrng_store(addy, val, ram_image);
  else if (addy >= VIRTIO_9P_BASE && addy < VIRTIO_9P_BASE + VIRTIO_9P_SIZE)
    v9p_store(addy, val, ram_image);
  else if (addy >= VIRTIO_INPUT_BASE && addy < VIRTIO_INPUT_BASE + VIRTIO_INPUT_SIZE)
    vinput_store(addy, val, ram_image);
  else if (addy >= RTC_GOLDFISH_BASE && addy < RTC_GOLDFISH_BASE + RTC_GOLDFISH_SIZE)
    rtc_goldfish_store(addy, val);
  else if (addy >= PLIC_BASE && addy < PLIC_BASE + PLIC_SIZE)
    plic_store(addy, val);
  return 0;
}
static uint32_t HandleControlLoad(uint32_t addy) {
  vblk_dev_t *bd;
  if (addy == 0x10000005) return 0x60 | IsKBHit();
  else if (addy == 0x10000000 && IsKBHit()) return ReadKBByte();
  else if (addy == 0x1100bffc) return core->timerh;
  else if (addy == 0x1100bff8) return core->timerl;
  else if ((bd = vblk_for_addr(addy)) != NULL)
    return vblk_load(bd, addy);
  else if (addy >= VIRTIO_NET_BASE && addy < VIRTIO_NET_BASE + VIRTIO_NET_SIZE)
    return vnet_load(addy);
  else if (addy >= VIRTIO_RNG_BASE && addy < VIRTIO_RNG_BASE + VIRTIO_RNG_SIZE)
    return vrng_load(addy);
  else if (addy >= VIRTIO_9P_BASE && addy < VIRTIO_9P_BASE + VIRTIO_9P_SIZE)
    return v9p_load(addy);
  else if (addy >= VIRTIO_INPUT_BASE && addy < VIRTIO_INPUT_BASE + VIRTIO_INPUT_SIZE)
    return vinput_load(addy);
  else if (addy >= RTC_GOLDFISH_BASE && addy < RTC_GOLDFISH_BASE + RTC_GOLDFISH_SIZE)
    return rtc_goldfish_load(addy);
  else if (addy >= PLIC_BASE && addy < PLIC_BASE + PLIC_SIZE)
    return plic_load(addy);
  return 0;
}
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value) {}
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno) { return 0; }


int main(int argc, char **argv) {
  gfxInitDefault();
  osSetSpeedupEnable(true);
  gfxSetDoubleBuffering(GFX_BOTTOM, false);

  term_init(&term_state);
  // Bottom screen: direct framebuffer (no consoleInit)

  draw_keyboard();

  term_printf("\x1b[2J");
  term_printf("Welcome to 3DS-CLI Linux Emulator\n");
  term_printf("Initializing mini-rv32ima...\n");
  term_state.dirty = true;
  uint64_t last_present_tick = 0;
  PresentTopScreen(&last_present_tick);

  while (ram_amt >= 8 * 1024 * 1024) {
    ram_image = malloc(ram_amt);
    if (ram_image) break;
    ram_amt -= 1024 * 1024;
  }

  if (!ram_image) {
    term_printf("Failed to allocate at least 8MB for RAM.\n");
    goto wait_exit;
  }

  term_printf("Allocated %lu bytes for RAM.\n", ram_amt);

  /* Open kernel Image (possibly a combined kernel+rootfs bundle) */
  FILE *f = fopen("sdmc:/Image", "rb");
  if (!f) f = fopen("Image", "rb");
  if (!f) {
    term_printf("Error: Could not open 'sdmc:/Image'.\n");
    term_printf("Please copy Image to sdmc:/\n");
    goto wait_exit;
  }

  fseek(f, 0, SEEK_END);
  long file_len = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint32_t rootfs_gz_len = 0, rootfs_raw_len = 0;
  long flen = ImageKernelLen(f, file_len, &rootfs_gz_len, &rootfs_raw_len);
  bool have_bundle = flen != file_len;

  /* Open rootfs disk image for virtio-blk, extracting it from the Image
     bundle first if it isn't on the SD card yet. */
  const char *disk_path = NULL;
  FILE *disk_file = OpenDiskFile(&disk_path);
  if (!disk_file && have_bundle) {
    if (ExtractEmbeddedRootfs(f, flen, rootfs_gz_len, rootfs_raw_len, &last_present_tick))
      disk_file = OpenDiskFile(&disk_path);
  }
  if (!disk_file) {
    term_printf("Error: Could not open rootfs.ext2.\n");
    term_printf("Please copy rootfs.ext2 to sdmc:/\n");
    fclose(f);
    goto wait_exit;
  }
  fseek(disk_file, 0, SEEK_END);
  uint64_t disk_size = (uint64_t)ftell(disk_file);
  fseek(disk_file, 0, SEEK_SET);
  plic_init();
  vblk_init(0, disk_file, disk_size, VIRTIO_BLK_BASE, PLIC_SRC_BLK);
  term_printf("Disk: %s (%lluMB)\n", disk_path, (unsigned long long)(disk_size >> 20));

  /* Swap is optional: if the file can't be created (full SD, read-only
     card), instance 1 keeps base == 0, nothing answers at 0x10005000, and
     the guest's init script falls back to zram. */
  swap_file = CreateSwapFile(&last_present_tick);
  if (swap_file) {
    vblk_init(1, swap_file, SWAP_SIZE_BYTES, VIRTIO_BLK2_BASE, PLIC_SRC_SWAP);
    term_printf("Swap: %luMB (%s)\n", (unsigned long)(SWAP_SIZE_BYTES >> 20), SWAP_PATH);
  } else {
    term_printf("Swap: unavailable (guest falls back to zram)\n");
  }

  vrng_init();
  term_printf("Hardware RNG: %s\n", vrng.ps_ready ? "ok" : "unavailable (fallback)");

  vnet_init();
  term_printf("Network: %s\n", vnet.soc_ready ? "ok (NAT via 3DS WiFi)" : "unavailable");

  /* Passthrough + sensors. The NAND trees need extended homebrew
     permissions; without them they're simply absent and the guest's
     mount of them fails rather than the whole device disappearing. */
  v9p_init();
  term_printf("Passthrough: sd hw%s%s\n",
              v9p_tree_ok[V9P_TREE_NAND] ? " nand" : "",
              v9p_tree_ok[V9P_TREE_TWL]  ? " twl"  : "");

  vinput_init();
  term_printf("Sensors: %s\n", hw.sensors ? "ok (accel, gyro, sliders)"
                                          : "unavailable");

  /* The RISC-V Linux Image header's image_load_offset field (8-byte LE at
     file offset 8) says how far into RAM the bootloader must place byte 0
     of this file - it's NOT always 0. MMU'd kernels (arch/riscv/kernel/
     head.S) use 0x400000 (4MB) for RV32; only the M-mode/NOMMU boot path
     this used to be uses 0. Ignoring this and always loading at RAM start
     (as this code did before real MMU support existed) puts the kernel's
     actual entry code 4MB away from where pc gets set, so the CPU starts
     executing whatever unrelated bytes happen to be at the front of the
     file instead - which decodes as an immediate illegal instruction. */
  uint64_t image_load_offset = 0;
  fseek(f, 8, SEEK_SET);
  if (fread(&image_load_offset, sizeof(image_load_offset), 1, f) != 1) image_load_offset = 0;
  fseek(f, 0, SEEK_SET);

  if (flen + (long)image_load_offset > (long)(ram_amt - 4*1024*1024)) {
    term_printf("Image too large for RAM.\n");
    fclose(f); fclose(disk_file); goto wait_exit;
  }
  memset(ram_image, 0, ram_amt);
  if (fread(ram_image + image_load_offset, (size_t)flen, 1, f) != 1) {
    term_printf("Error: Could not read Image.\n");
    fclose(f); fclose(disk_file); goto wait_exit;
  }
  fclose(f);

  {
    /* Linux's fdt_check_header() requires the DTB physical address to be
       8-byte aligned (it returns -FDT_ERR_ALIGNMENT otherwise), which
       silently skips the entire device-tree scan, including memory
       detection. sizeof(default64mbdtb)/sizeof(struct MiniRV32IMAState)
       aren't multiples of 8, so this must be rounded down explicitly. */
    uint32_t dtb_ptr = (ram_amt - sizeof(default64mbdtb) - sizeof(struct MiniRV32IMAState)) & ~7u;
    memcpy(ram_image + dtb_ptr, default64mbdtb, sizeof(default64mbdtb));

    /* Patch the RAM-size word in the DTB's memory node with the actual usable size.
       dtb_ptr (as big-endian) is the number of bytes Linux may use before the DTB. */
    uint32_t *dtb = (uint32_t *)(ram_image + dtb_ptr);
    uint32_t *patch = (uint32_t *)((uint8_t *)dtb + DTB_MEM_SIZE_OFFSET);
    uint32_t vr = dtb_ptr;
    *patch = (vr>>24)|((vr>>16&0xff)<<8)|((vr>>8&0xff)<<16)|((vr&0xff)<<24);

    core = (struct MiniRV32IMAState *)(ram_image + ram_amt - sizeof(struct MiniRV32IMAState));
    core->pc = MINIRV32_RAM_IMAGE_OFFSET + (uint32_t)image_load_offset;
    core->regs[10] = 0x00; // hartid
    core->regs[11] = dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET) : 0; // dtb pointer

    /* Boot directly into S-mode, mirroring the register/CSR state a real
       firmware handoff (OpenSBI's `mret` into the kernel) would leave
       behind - see vendor/mini-rv32ima-mmu's file header for why there's
       no actual M-mode boot code here. medeleg delegates every exception
       the kernel needs to handle itself (including U-mode ecalls, i.e.
       syscalls); ecalls from S-mode (SBI calls, cause 9) are deliberately
       left un-delegated, so they always reach HandleException to be
       serviced. mideleg delegates the three S-level interrupt causes.
       mstatus.MIE and mie.MTIE stay permanently set so the real machine
       timer interrupt can fire at all and get forwarded to the guest as
       an STIP supervisor timer interrupt (see HandleException). */
    core->extraflags |= 1; // S-mode
    core->medeleg = (1u<<0)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<12)|(1u<<13)|(1u<<15);
    core->mideleg = (1u<<1)|(1u<<5)|(1u<<9); // SSI, STI, SEI
    core->mstatus = (1u<<3); // MIE
    core->mie = (1u<<7); // MTIE
  }

  dbg_log_file = fopen("sdmc:/3ds-cli-debug.log", "w");
  if (!dbg_log_file) dbg_log_file = fopen("3ds-cli-debug.log", "w");
  uart_log_file = fopen("sdmc:/3ds-cli-console.log", "w");
  if (dbg_log_file) {
    fprintf(dbg_log_file, "[host] vnet soc_ready=%d\n", (int)vnet.soc_ready);
    fflush(dbg_log_file);
  }

  term_printf("Booting Linux... This may take a while.\n");
  term_state.dirty = true;
  PresentTopScreen(&last_present_tick);

  bool touch_held = false;
  uint64_t last_tick = svcGetSystemTick();

  while (aptMainLoop()) {
    int ret = 0;

    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();
    if (kDown & KEY_START) break;

    // Zoom controls (L/Y = zoom out, R/X = zoom in, always both axes equally)
    if (kDown & KEY_L || kDown & KEY_Y) {
      int z = (term_state.zoom_x > 1) ? term_state.zoom_x - 1 : 1;
      term_state.zoom_x = z;
      term_state.zoom_y = z;
      term_state.dirty = true;
    }
    if (kDown & KEY_R || kDown & KEY_X) {
      int z = (term_state.zoom_x < 5) ? term_state.zoom_x + 1 : 5;
      term_state.zoom_x = z;
      term_state.zoom_y = z;
      term_state.dirty = true;
    }
    if (kDown & KEY_ZL) {
      term_state.auto_track = !term_state.auto_track;
      if (term_state.auto_track) term_state.scroll_y = 0; // snap back to live
      term_state.dirty = true;
    }
    if (kDown & KEY_ZR) {
      term_state.use_5x7 = !term_state.use_5x7;
      term_state.dirty = true;
    }

    // Viewport panning with Circle Pad
    static int pan_cooldown_x = 0;
    static int pan_cooldown_y = 0;
    circlePosition cpos;
    hidCircleRead(&cpos);

    if (abs(cpos.dx) > 40) {
      term_state.auto_track = false;
      if (pan_cooldown_x <= 0) {
        if (cpos.dx > 40) term_state.scroll_x++;
        else if (cpos.dx < -40) term_state.scroll_x--;
        pan_cooldown_x = 4;
        term_state.dirty = true;
      } else {
        pan_cooldown_x--;
      }
    } else {
      pan_cooldown_x = 0;
    }

    if (abs(cpos.dy) > 40) {
      term_state.auto_track = false;
      if (pan_cooldown_y <= 0) {
        if (cpos.dy > 40) term_state.scroll_y--;
        else if (cpos.dy < -40) term_state.scroll_y++;
        pan_cooldown_y = 4;
        term_state.dirty = true;
      } else {
        pan_cooldown_y--;
      }
    } else {
      pan_cooldown_y = 0;
    }

    // Cursor blink check
    static bool last_blink_on = false;
    bool blink_on = (svcGetSystemTick() / (268000LL * 500)) % 2 == 0;
    if (blink_on != last_blink_on) {
      term_state.dirty = true;
      last_blink_on = blink_on;
    }

    // D-pad arrows
    if (kDown & KEY_DUP)    rx_push_str("\x1b[A");
    if (kDown & KEY_DDOWN)  rx_push_str("\x1b[B");
    if (kDown & KEY_DRIGHT) rx_push_str("\x1b[C");
    if (kDown & KEY_DLEFT)  rx_push_str("\x1b[D");

    // Touch keyboard
    if (kHeld & KEY_TOUCH) {
      if (!touch_held) {
        touch_held = true;
        touchPosition tp;
        hidTouchRead(&tp);

        int kb_end_y = KB_Y0 + 5 * (KEY_H + GAP);
        if (tp.py >= KB_Y0 && tp.py < kb_end_y) {
          int r = (tp.py - KB_Y0) / (KEY_H + GAP);
          int c = (tp.px - KB_X0) / (KEY_W + GAP);
          if (c < 0) c = 0;
          if (c > 9) c = 9;
          if (r >= 0 && r < 5) {
            c = normalize_key_col(r, c);
            pressed_key_r = r;
            pressed_key_c = c;
            const char (*m)[10] = get_keymap();
            char key = m[r][c];
            unsigned char uk = (unsigned char)key;
            bool redraw = true;

            if (uk == K_SHIFT)     { kb_layer = (kb_layer==1)?0:1; redraw=true; }
            else if (uk == K_SYM)  { kb_layer = 2; redraw=true; }
            else if (uk == K_ABC)  { kb_layer = 0; redraw=true; }
            else if (uk == K_SYM2) { kb_layer = 3; redraw=true; }
            else if (uk == K_CTRL) { ctrl_active = !ctrl_active; redraw=true; }
            else if (uk == (unsigned char)K_TAB)   rx_push('\t');
            else if (uk == K_ESC)                  rx_push(27);
            else if (uk == (unsigned char)K_ENTER)  rx_push('\n');
            else if (uk == (unsigned char)K_BKSP)   rx_push(127);
            else if (uk == K_SPACE)                 rx_push(' ');
            else {
              if (ctrl_active) {
                if (key>='a'&&key<='z') rx_push(key-'a'+1);
                else if (key>='A'&&key<='Z') rx_push(key-'A'+1);
                else rx_push(key);
                ctrl_active = false; redraw = true;
              } else {
                rx_push(key);
                if (kb_layer == 1) { kb_layer = 0; redraw = true; }
              }
            }
            if (redraw) draw_keyboard();
          }
        }
      }
    } else {
      if (touch_held || pressed_key_r != -1) {
        touch_held = false;
        pressed_key_r = -1;
        pressed_key_c = -1;
        draw_keyboard();
      }
    }

    // Emulator steps
    //
    // dbg_log_file below writes a ring buffer of the last DBG_RING_N
    // distinct (pc,mcause) transitions plus a full register dump to
    // sdmc:/3ds-cli-debug.log whenever execution is detected as stuck
    // (200 consecutive chunks with zero forward progress), re-arming once
    // forward progress resumes. Cheap enough to leave enabled, and it has
    // paid for itself repeatedly when a guest-side hang needed diagnosing.
    uint64_t cur = svcGetSystemTick();
    uint32_t us = (cur - last_tick) / TICKS_PER_US;
    last_tick = cur;
    uint64_t frame_start = cur;
    uint32_t elapsed_us = us;
    static uint32_t dbg_step = 0;
    static uint32_t dbg_last_pc = 0xffffffffu, dbg_last_mc = 0xffffffffu;
    static uint32_t dbg_stuck_count = 0;
    static bool dbg_dumped = false;
#define DBG_RING_N 512
    typedef struct { uint32_t step, pc, mc, ep, tv, ra, sp, tp, t4, gp, a0, mstatus; } dbg_rec_t;
    static dbg_rec_t dbg_ring[DBG_RING_N];
    static int dbg_ring_pos = 0;
    do {
      ret = MiniRV32IMAStep(core, ram_image, 0, elapsed_us, EMU_STEP_CHUNK);
      elapsed_us = 0;
      dbg_step++;
      bool changed = core->pc != dbg_last_pc || core->mcause != dbg_last_mc;
      if (changed) {
        dbg_ring[dbg_ring_pos].step = dbg_step;
        dbg_ring[dbg_ring_pos].pc = core->pc;
        dbg_ring[dbg_ring_pos].mc = core->mcause;
        dbg_ring[dbg_ring_pos].ep = core->mepc;
        dbg_ring[dbg_ring_pos].tv = core->mtval;
        dbg_ring[dbg_ring_pos].ra = core->regs[1];
        dbg_ring[dbg_ring_pos].sp = core->regs[2];
        dbg_ring[dbg_ring_pos].gp = core->regs[3];
        dbg_ring[dbg_ring_pos].tp = core->regs[4];
        dbg_ring[dbg_ring_pos].a0 = core->regs[10];
        dbg_ring[dbg_ring_pos].t4 = core->regs[29];
        dbg_ring[dbg_ring_pos].mstatus = core->mstatus;
        dbg_ring_pos = (dbg_ring_pos + 1) % DBG_RING_N;
        dbg_stuck_count = 0;
        dbg_dumped = false; // Forward progress resumed: re-arm for the next stall.
      } else {
        dbg_stuck_count++;
      }
      if (dbg_step <= 5 || dbg_step == 10 || dbg_step % 500 == 0 || changed) {
        /* Deliberately NOT printed to the terminal: this shares the top
           screen with the guest kernel's own console output, and spamming
           it here was scrolling away real kernel boot/panic text before
           it could ever be seen. File-only (dbg_log_file below). */
        dbg_last_pc = core->pc;
        dbg_last_mc = core->mcause;
      }
      /* Once the same (pc,mcause) has been the outcome of ~200 consecutive
         chunk calls with zero forward progress, we're permanently stuck:
         dump the ring buffer of the last DBG_RING_N distinct transitions
         (i.e. the run-up to the freeze) plus a full register snapshot. */
      if (!dbg_dumped && dbg_stuck_count == 200 && dbg_log_file) {
        dbg_dumped = true;
        fprintf(dbg_log_file, "=== STUCK at step %lu, dumping last %d transitions ===\n",
          (unsigned long)dbg_step, DBG_RING_N);
        for (int i = 0; i < DBG_RING_N; i++) {
          dbg_rec_t *r = &dbg_ring[(dbg_ring_pos + i) % DBG_RING_N];
          if (r->step == 0) continue;
          fprintf(dbg_log_file, "[%lu] pc=%08lx mc=%lx ep=%08lx tv=%08lx ra=%08lx sp=%08lx gp=%08lx tp=%08lx a0=%08lx t4=%08lx mstatus=%08lx\n",
            (unsigned long)r->step, (unsigned long)r->pc, (unsigned long)r->mc,
            (unsigned long)r->ep, (unsigned long)r->tv, (unsigned long)r->ra,
            (unsigned long)r->sp, (unsigned long)r->gp, (unsigned long)r->tp,
            (unsigned long)r->a0, (unsigned long)r->t4, (unsigned long)r->mstatus);
        }
        fprintf(dbg_log_file, "=== FULL REGDUMP at step %lu ===\n", (unsigned long)dbg_step);
        for (int ri = 0; ri < 32; ri++) {
          fprintf(dbg_log_file, "x%-2d=%08lx%s", ri, (unsigned long)core->regs[ri], (ri % 4 == 3) ? "\n" : "  ");
        }
        fprintf(dbg_log_file, "\npc=%08lx mepc=%08lx mtval=%08lx mcause=%08lx mscratch=%08lx mtvec=%08lx mstatus=%08lx mie=%08lx mip=%08lx\n",
          (unsigned long)core->pc, (unsigned long)core->mepc, (unsigned long)core->mtval,
          (unsigned long)core->mcause, (unsigned long)core->mscratch, (unsigned long)core->mtvec,
          (unsigned long)core->mstatus, (unsigned long)core->mie, (unsigned long)core->mip);
        fprintf(dbg_log_file, "satp=%08lx sepc=%08lx stval=%08lx scause=%08lx stvec=%08lx sscratch=%08lx priv=%lu\n",
          (unsigned long)core->satp, (unsigned long)core->sepc, (unsigned long)core->stval,
          (unsigned long)core->scause, (unsigned long)core->stvec, (unsigned long)core->sscratch,
          (unsigned long)(core->extraflags & 3));
        fprintf(dbg_log_file, "=== END ===\n");
        fflush(dbg_log_file);
      }
    } while (ret != 1 &&
             ret != 0x5555 &&
             ret != 3 &&
             !sbi_shutdown_requested &&
             svcGetSystemTick() - frame_start < (uint64_t)EMU_RUN_BUDGET_US * TICKS_PER_US);

    vnet_poll(ram_image);
    /* Sensor sampling piggybacks on the hidScanInput() already done at the
       top of this loop, so it costs a handful of comparisons per frame and
       naturally runs at the console's own refresh rate. */
    vinput_poll(ram_image);

    if (ret == 0x5555 || ret == 3 || sbi_shutdown_requested) {
      if (ret == 3) term_printf("Emulator fault!\n");
      term_state.dirty = true;
      PresentTopScreen(&last_present_tick);
      break;
    }

    if (ret == 1 || TimeSinceUs(last_present_tick, TOP_REFRESH_INTERVAL_US)) {
      if (term_state.dirty) {
        PresentTopScreen(&last_present_tick);
      }
    }

    if (ret == 1) {
      svcSleepThread(1000000LL);
    }
  }

  if (disk_file) fclose(disk_file);
  CloseSwapFile();
  v9p_exit();
  if (vnet.soc_ready) socExit();
  if (vrng.ps_ready) psExit();
  free(ram_image);
  gfxExit();
  return 0;

wait_exit:
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    u8 *top_fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    term_draw(&term_state, top_fb);
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
  }
  CloseSwapFile();
  free(ram_image);
  gfxExit();
  return -1;
}
