#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"

PrintConsole topScreen;

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
    fb_string(4, 21, "D-pad=arrows  START=quit", CLR_TXTD_R, CLR_TXTD_G, CLR_TXTD_B, 1);
  }
  fb_string(220, 21, "root@3ds", 80, 180, 120, 1);

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

  // Bottom bar: D-pad hint
  fb_string(4, 230, "D-pad", 60, 60, 70, 1);
  fb_string(48, 230, "arrows", 80, 140, 180, 1);
  fb_string(240, 230, "START", 60, 60, 70, 1);
  fb_string(284, 230, "quit", 180, 80, 80, 1);

  gfxFlushBuffers();
}

// ---------------------------------------------------------
// mini-rv32ima Integration
// ---------------------------------------------------------
#include "default64mbdtc.h"

uint32_t ram_amt = 32 * 1024 * 1024;
uint8_t *ram_image = 0;
struct MiniRV32IMAState *core;

#define TICKS_PER_US 268
#define EMU_FRAME_BUDGET_US 15000
#define EMU_STEP_CHUNK 50000

char rx_buf[256];
int rx_head = 0, rx_tail = 0;

static char tx_buf[2048];
static int tx_len = 0;

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

static void FlushUART(void) {
  if (!tx_len) return;
  consoleSelect(&topScreen);
  fwrite(tx_buf, 1, tx_len, stdout);
  tx_len = 0;
}

static void WriteUARTByte(char c) {
  tx_buf[tx_len++] = c;
  if (tx_len == (int)sizeof(tx_buf)) FlushUART();
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

static uint32_t HandleException(uint32_t ir, uint32_t retval) { return retval; }
static uint32_t HandleControlStore(uint32_t addy, uint32_t val) {
  if (addy == 0x10000000) WriteUARTByte((char)val);
  else if (addy == 0x11004004) core->timermatchh = val;
  else if (addy == 0x11004000) core->timermatchl = val;
  else if (addy == 0x11100000) { core->pc += 4; return val; }
  return 0;
}
static uint32_t HandleControlLoad(uint32_t addy) {
  if (addy == 0x10000005) return 0x60 | IsKBHit();
  else if (addy == 0x10000000 && IsKBHit()) return ReadKBByte();
  else if (addy == 0x1100bffc) return core->timerh;
  else if (addy == 0x1100bff8) return core->timerl;
  return 0;
}
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value) {}
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno) { return 0; }

int main(int argc, char **argv) {
  gfxInitDefault();
  gfxSetDoubleBuffering(GFX_BOTTOM, false);
  consoleInit(GFX_TOP, &topScreen);
  // Bottom screen: direct framebuffer (no consoleInit)

  draw_keyboard();

  consoleSelect(&topScreen);
  printf("\x1b[2J");
  printf("Welcome to 3DS-CLI Linux Emulator\n");
  printf("Initializing mini-rv32ima...\n");
  ram_image = malloc(ram_amt);
  if (!ram_image) {
    printf("Failed to allocate %lu bytes for RAM.\n", ram_amt);
    goto wait_exit;
  }

  FILE *f = fopen("sdmc:/Image", "rb");
  if (!f) f = fopen("Image", "rb");
  if (!f) {
    printf("Error: Could not open 'sdmc:/Image'.\n");
    printf("Please copy the Image file to your SD card.\n");
    goto wait_exit;
  }

  fseek(f, 0, SEEK_END);
  long flen = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (flen > ram_amt - 4*1024*1024) {
    printf("Image too large for RAM.\n");
    fclose(f); goto wait_exit;
  }
  memset(ram_image, 0, ram_amt);
  fread(ram_image, flen, 1, f);
  fclose(f);

  uint32_t dtb_ptr = ram_amt - sizeof(default64mbdtb) - sizeof(struct MiniRV32IMAState);
  memcpy(ram_image + dtb_ptr, default64mbdtb, sizeof(default64mbdtb));
  uint32_t *dtb = (uint32_t *)(ram_image + dtb_ptr);
  if (dtb[0x13c/4] == 0x00c0ff03) {
    uint32_t vr = dtb_ptr;
    dtb[0x13c/4] = (vr>>24)|((vr>>16&0xff)<<8)|((vr>>8&0xff)<<16)|((vr&0xff)<<24);
  }

  core = (struct MiniRV32IMAState *)(ram_image + ram_amt - sizeof(struct MiniRV32IMAState));
  core->pc = MINIRV32_RAM_IMAGE_OFFSET;
  core->regs[10] = 0x00;
  core->regs[11] = dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET) : 0;
  core->extraflags |= 3;

  printf("Booting Linux... This may take a while.\n");

  bool touch_held = false;
  uint64_t last_tick = svcGetSystemTick();

  while (aptMainLoop()) {
    int ret = 0;

    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();
    if (kDown & KEY_START) break;

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
    uint64_t cur = svcGetSystemTick();
    uint32_t us = (cur - last_tick) / TICKS_PER_US;
    last_tick = cur;
    uint64_t frame_start = cur;
    uint32_t elapsed_us = us;
    do {
      ret = MiniRV32IMAStep(core, ram_image, 0, elapsed_us, EMU_STEP_CHUNK);
      elapsed_us = 0;
    } while (ret != 1 &&
             ret != 0x5555 &&
             ret != 3 &&
             svcGetSystemTick() - frame_start < (uint64_t)EMU_FRAME_BUDGET_US * TICKS_PER_US);

    FlushUART();
    if (ret == 0x5555 || ret == 3) {
      if (ret == 3) printf("Emulator fault!\n");
      break;
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  free(ram_image);
  gfxExit();
  return 0;

wait_exit:
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
  }
  free(ram_image);
  gfxExit();
  return -1;
}
