#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PrintConsole topScreen, bottomScreen;

// ---- Keyboard Layer System (v1 was unusably bad) ----
// Layer 0: lowercase letters
// Layer 1: uppercase letters (Shift)
// Layer 2: numbers & symbols page 1
// Layer 3: more symbols page 2

// Special key codes (non-printable, used for internal dispatch by me)
#define K_SHIFT 1
#define K_SYM 2
#define K_ABC 3
#define K_SYM2 4
#define K_CTRL 5
#define K_TAB '\t'
#define K_ESC 27
#define K_ENTER '\n'
#define K_BKSP '\b'
#define K_SPACE ' '

// 5 rows x 10 cols per layer
// Row 0: number / symbol top row
// Row 1: qwerty row
// Row 2: asdf row
// Row 3: shift + zxcv row
// Row 4: modifiers + space + enter + bksp

const char keymap_lower[5][10] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\''},
    {K_SHIFT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.'},
    {K_TAB, K_ESC, K_SYM, K_SPACE, K_SPACE, K_SPACE, K_SPACE, K_CTRL, K_ENTER,
     K_BKSP}};

const char keymap_upper[5][10] = {
    {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'},
    {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
    {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '"'},
    {K_SHIFT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':'},
    {K_TAB, K_ESC, K_SYM, K_SPACE, K_SPACE, K_SPACE, K_SPACE, K_CTRL, K_ENTER,
     K_BKSP}};

const char keymap_sym1[5][10] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'+', '-', '*', '/', '=', '|', '\\', '`', '~', '_'},
    {'@', '#', '$', '%', '^', '&', '(', ')', '[', ']'},
    {K_SYM2, '{', '}', '<', '>', '?', '!', ':', ';', '"'},
    {K_TAB, K_ESC, K_ABC, K_SPACE, K_SPACE, K_SPACE, K_SPACE, K_CTRL, K_ENTER,
     K_BKSP}};

const char keymap_sym2[5][10] = {
    {'`', '~', '|', '\\', '_', '-', '+', '=', '.', ','},
    {'[', ']', '{', '}', '<', '>', '(', ')', '/', '?'},
    {'!', '@', '#', '$', '%', '^', '&', '*', '\'', '"'},
    {K_ABC, ':', ';', '-', '_', '+', '=', '\\', '|', '~'},
    {K_TAB, K_ESC, K_ABC, K_SPACE, K_SPACE, K_SPACE, K_SPACE, K_CTRL, K_ENTER,
     K_BKSP}};

int kb_layer = 0; // 0=lower, 1=upper, 2=sym1, 3=sym2
bool ctrl_active = false;

const char (*get_current_keymap(void))[10] {
  switch (kb_layer) {
  case 1:
    return keymap_upper;
  case 2:
    return keymap_sym1;
  case 3:
    return keymap_sym2;
  default:
    return keymap_lower;
  }
}

bool is_special_key(char key) {
  unsigned char k = (unsigned char)key;
  return (k == K_SHIFT || k == K_SYM || k == K_ABC || k == K_SYM2 ||
          k == K_CTRL);
}

// Keyboard layout constants
// 3DS bottom screen console: 40 cols x 30 rows (8x8 px font, 320x240 screen)
// Keyboard: 5 rows of keys, each key 4 chars wide x 3 text rows tall
// Rows start at text row 6 (leaving rows 1-5 for status/info)
// Total keyboard height: 5 * 3 = 15 rows (rows 6-20)
// Remaining rows 21-30 for padding / d-pad hint
#define KB_START_ROW 7
#define KB_KEY_H 3
#define KB_KEY_W 4

// Pixel coordinates for touch detection
// text row N -> pixel y = (N-1) * 8
#define KB_START_PX_Y ((KB_START_ROW - 1) * 8) // 48
#define KB_KEY_PX_H (KB_KEY_H * 8)             // 24
#define KB_KEY_PX_W (KB_KEY_W * 8)             // 32

void draw_keyboard() {
  consoleSelect(&bottomScreen);
  printf("\x1b[2J"); // Clear screen

  // ---- Status bar (rows 1-3) ----
  printf("\x1b[1;1H");
  printf("\x1b[37;44m 3ds-cli \x1b[0m ");
  printf("\x1b[36madabit.org\x1b[0m");

  // Layer indicator on the right
  const char *layer_tag;
  switch (kb_layer) {
  case 0:
    layer_tag = "\x1b[42;30m abc \x1b[0m";
    break;
  case 1:
    layer_tag = "\x1b[42;30m ABC \x1b[0m";
    break;
  case 2:
    layer_tag = "\x1b[43;30m ?#1 \x1b[0m";
    break;
  case 3:
    layer_tag = "\x1b[43;30m #+= \x1b[0m";
    break;
  default:
    layer_tag = "";
    break;
  }
  printf("\x1b[1;34H%s", layer_tag);

  // Info line
  printf("\x1b[3;1H\x1b[90mlogin: root | START=quit\x1b[0m");
  if (ctrl_active) {
    printf(
        "\x1b[4;1H\x1b[41;37m CTRL \x1b[0m\x1b[90m active - tap a key\x1b[0m");
  } else {
    printf("\x1b[4;1H\x1b[90m D-pad = arrow keys       \x1b[0m");
  }

  const char (*map)[10] = get_current_keymap();

  // ---- Draw keyboard rows 0-3 (character keys) ----
  for (int r = 0; r < 4; r++) {
    int tr = KB_START_ROW + (r * KB_KEY_H);

    for (int c = 0; c < 10; c++) {
      char key = map[r][c];
      int tc = c * KB_KEY_W + 1; // 1-indexed text column
      unsigned char uk = (unsigned char)key;

      if (is_special_key(key)) {
        // ---- Special modifier keys (Shift, Sym, ABC, Sym2, Ctrl) ----
        if (uk == K_SHIFT) {
          // Shift key
          if (kb_layer == 1) {
            // Active: green background
            printf("\x1b[%d;%dH\x1b[42;30m", tr, tc);
            printf(" ^  ");
            printf("\x1b[%d;%dH", tr + 1, tc);
            printf("SHF ");
            printf("\x1b[%d;%dH", tr + 2, tc);
            printf("    ");
            printf("\x1b[0m");
          } else {
            printf("\x1b[%d;%dH\x1b[100;37m", tr, tc);
            printf(" ^  ");
            printf("\x1b[%d;%dH", tr + 1, tc);
            printf("SHF ");
            printf("\x1b[%d;%dH", tr + 2, tc);
            printf("    ");
            printf("\x1b[0m");
          }
        } else if (uk == K_SYM) {
          printf("\x1b[%d;%dH\x1b[43;30m", tr, tc);
          printf("?#1 ");
          printf("\x1b[%d;%dH", tr + 1, tc);
          printf("    ");
          printf("\x1b[%d;%dH", tr + 2, tc);
          printf("    ");
          printf("\x1b[0m");
        } else if (uk == K_ABC) {
          printf("\x1b[%d;%dH\x1b[44;37m", tr, tc);
          printf("ABC ");
          printf("\x1b[%d;%dH", tr + 1, tc);
          printf("    ");
          printf("\x1b[%d;%dH", tr + 2, tc);
          printf("    ");
          printf("\x1b[0m");
        } else if (uk == K_SYM2) {
          printf("\x1b[%d;%dH\x1b[43;30m", tr, tc);
          printf("#+= ");
          printf("\x1b[%d;%dH", tr + 1, tc);
          printf("    ");
          printf("\x1b[%d;%dH", tr + 2, tc);
          printf("    ");
          printf("\x1b[0m");
        } else if (uk == K_CTRL) {
          if (ctrl_active) {
            printf("\x1b[%d;%dH\x1b[41;37m", tr, tc);
          } else {
            printf("\x1b[%d;%dH\x1b[100;37m", tr, tc);
          }
          printf("CTL ");
          printf("\x1b[%d;%dH", tr + 1, tc);
          printf("    ");
          printf("\x1b[%d;%dH", tr + 2, tc);
          printf("    ");
          printf("\x1b[0m");
        }
      } else {
        // ---- Normal character key ----
        // Top line: thin separator
        printf("\x1b[%d;%dH\x1b[90m", tr, tc);
        printf("----");
        printf("\x1b[0m");

        // Middle line: the character itself
        if (r == 0) {
          // Number row: yellow/gold color
          printf("\x1b[%d;%dH\x1b[33;40m", tr + 1, tc);
        } else {
          // Letter rows: white on dark
          printf("\x1b[%d;%dH\x1b[97;40m", tr + 1, tc);
        }
        printf(" %c  ", key);
        printf("\x1b[0m");

        // Bottom line: thin separator
        printf("\x1b[%d;%dH\x1b[90m", tr + 2, tc);
        printf("----");
        printf("\x1b[0m");
      }
    }
  }

  // ---- Draw row 4: bottom bar (TAB ESC ?#1 ----SPACE---- CTL <-  <X]) ----
  int tr = KB_START_ROW + (4 * KB_KEY_H);

  // TAB (col 0, 4 chars wide)
  printf("\x1b[%d;1H\x1b[100;36m", tr);
  printf("TAB ");
  printf("\x1b[%d;1H", tr + 1);
  printf("    ");
  printf("\x1b[%d;1H", tr + 2);
  printf("    ");
  printf("\x1b[0m");

  // ESC (col 1, 4 chars wide)
  printf("\x1b[%d;5H\x1b[100;35m", tr);
  printf("ESC ");
  printf("\x1b[%d;5H", tr + 1);
  printf("    ");
  printf("\x1b[%d;5H", tr + 2);
  printf("    ");
  printf("\x1b[0m");

  // SYM/ABC (col 2, 4 chars wide)
  if (kb_layer >= 2) {
    printf("\x1b[%d;9H\x1b[44;37m", tr);
    printf("ABC ");
  } else {
    printf("\x1b[%d;9H\x1b[43;30m", tr);
    printf("?#1 ");
  }
  printf("\x1b[%d;9H", tr + 1);
  printf("    ");
  printf("\x1b[%d;9H", tr + 2);
  printf("    ");
  printf("\x1b[0m");

  // SPACE (cols 3-6, 16 chars wide)
  printf("\x1b[%d;13H\x1b[47;30m", tr);
  printf("                ");
  printf("\x1b[%d;13H\x1b[47;30m", tr + 1);
  printf("   S P A C E    ");
  printf("\x1b[%d;13H\x1b[47;30m", tr + 2);
  printf("                ");
  printf("\x1b[0m");

  // CTRL (col 7, 4 chars wide)
  if (ctrl_active) {
    printf("\x1b[%d;29H\x1b[41;37m", tr);
  } else {
    printf("\x1b[%d;29H\x1b[100;37m", tr);
  }
  printf("CTL ");
  printf("\x1b[%d;29H", tr + 1);
  printf("    ");
  printf("\x1b[%d;29H", tr + 2);
  printf("    ");
  printf("\x1b[0m");

  // ENTER (col 8, 4 chars wide)
  printf("\x1b[%d;33H\x1b[42;30m", tr);
  printf(" <- ");
  printf("\x1b[%d;33H\x1b[42;30m", tr + 1);
  printf("ENT ");
  printf("\x1b[%d;33H\x1b[42;30m", tr + 2);
  printf("    ");
  printf("\x1b[0m");

  // BKSP (col 9, 4 chars wide)
  printf("\x1b[%d;37H\x1b[41;37m", tr);
  printf(" <- ");
  printf("\x1b[%d;37H\x1b[41;37m", tr + 1);
  printf("DEL ");
  printf("\x1b[%d;37H\x1b[41;37m", tr + 2);
  printf("    ");
  printf("\x1b[0m");

  // ---- Bottom hint ----
  printf("\x1b[30;1H\x1b[90m [D-pad]\x1b[36m arrows  \x1b[90m[START]\x1b[36m "
         "quit\x1b[0m");
}

// ---------------------------------------------------------
// mini-rv32ima Integration
// ---------------------------------------------------------
#include "default64mbdtc.h"

uint32_t ram_amt = 32 * 1024 * 1024; // Allocate 32MB to be safe on O3DS
uint8_t *ram_image = 0;
struct MiniRV32IMAState *core;

// UART RX Buffer (Keyboard input)
char rx_buf[256];
int rx_head = 0;
int rx_tail = 0;

static int IsKBHit() { return (rx_head != rx_tail); }

static int ReadKBByte() {
  if (rx_head == rx_tail)
    return -1;
  char c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % 256;
  return c;
}

// Push a single byte into the rx buffer
static void rx_push(char c) {
  int next_head = (rx_head + 1) % 256;
  if (next_head != rx_tail) {
    rx_buf[rx_head] = c;
    rx_head = next_head;
  }
}

// Push a string (for VT100 escape sequences like arrow keys)
static void rx_push_str(const char *s) {
  while (*s) {
    rx_push(*s);
    s++;
  }
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
#define MINIRV32_POSTEXEC(pc, ir, retval)                                      \
  {                                                                            \
    if (retval > 0) {                                                          \
      retval = HandleException(ir, retval);                                    \
    }                                                                          \
  }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val)                           \
  if (HandleControlStore(addy, val))                                           \
    return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval)                           \
  rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value)                                  \
  HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value)                                   \
  value = HandleOtherCSRRead(image, csrno);

#include "mini-rv32ima.h"

static uint32_t HandleException(uint32_t ir, uint32_t retval) { return retval; }

static uint32_t HandleControlStore(uint32_t addy, uint32_t val) {
  if (addy == 0x10000000) { // UART 8250 / 16550 Data Buffer
    consoleSelect(&topScreen);
    printf("%c", (int)val);
  } else if (addy == 0x11004004) // CLNT
    core->timermatchh = val;
  else if (addy == 0x11004000) // CLNT
    core->timermatchl = val;
  else if (addy == 0x11100000) { // SYSCON (poweroff)
    core->pc = core->pc + 4;
    return val;
  }
  return 0;
}

static uint32_t HandleControlLoad(uint32_t addy) {
  if (addy == 0x10000005)
    return 0x60 | IsKBHit();
  else if (addy == 0x10000000 && IsKBHit())
    return ReadKBByte();
  else if (addy == 0x1100bffc)
    return core->timerh;
  else if (addy == 0x1100bff8)
    return core->timerl;
  return 0;
}

static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno,
                                uint32_t value) {}
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno) { return 0; }

int main(int argc, char **argv) {
  gfxInitDefault();
  consoleInit(GFX_TOP, &topScreen);
  consoleInit(GFX_BOTTOM, &bottomScreen);

  draw_keyboard();

  consoleSelect(&topScreen);
  printf("\x1b[2J");
  printf("Welcome to 3DS-CLI Linux Emulator\n");
  printf("Initializing mini-rv32ima...\n");
  ram_image = malloc(ram_amt);
  if (!ram_image) {
    printf("Failed to allocate %lu bytes for RAM.\n", ram_amt);
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_START)
        break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    gfxExit();
    return -1;
  }

  FILE *f = fopen("sdmc:/Image", "rb");
  if (!f)
    f = fopen("Image", "rb"); // Fallback for local testing

  if (!f) {
    printf("Error: Could not open 'sdmc:/Image'.\n");
    printf("Please copy the Image file to your SD card.\n");
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_START)
        break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    free(ram_image);
    gfxExit();
    return -1;
  }

  fseek(f, 0, SEEK_END);
  long flen = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (flen > ram_amt - 4 * 1024 * 1024) { // Leave room for DTB and core
    printf("Image too large for RAM.\n");
    fclose(f);
    free(ram_image);
    gfxExit();
    return -1;
  }

  memset(ram_image, 0, ram_amt);
  fread(ram_image, flen, 1, f);
  fclose(f);

  uint32_t dtb_ptr =
      ram_amt - sizeof(default64mbdtb) - sizeof(struct MiniRV32IMAState);
  memcpy(ram_image + dtb_ptr, default64mbdtb, sizeof(default64mbdtb));

  // Update system ram size in DTB to match our actual ram_amt
  uint32_t *dtb = (uint32_t *)(ram_image + dtb_ptr);
  if (dtb[0x13c / 4] == 0x00c0ff03) {
    uint32_t validram = dtb_ptr;
    dtb[0x13c / 4] = (validram >> 24) | (((validram >> 16) & 0xff) << 8) |
                     (((validram >> 8) & 0xff) << 16) |
                     ((validram & 0xff) << 24);
  }

  core = (struct MiniRV32IMAState *)(ram_image + ram_amt -
                                     sizeof(struct MiniRV32IMAState));
  core->pc = MINIRV32_RAM_IMAGE_OFFSET;
  core->regs[10] = 0x00; // hart ID
  core->regs[11] =
      dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET) : 0; // dtb_pa
  core->extraflags |= 3;                                   // Machine-mode

  printf("Booting Linux... This may take a while.\n");

  bool touch_held = false;
  uint64_t last_tick = svcGetSystemTick();

  while (aptMainLoop()) {
    hidScanInput();

    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();

    if (kDown & KEY_START)
      break;

    // D-pad -> VT100 arrow key escape sequences
    if (kDown & KEY_DUP)
      rx_push_str("\x1b[A");
    if (kDown & KEY_DDOWN)
      rx_push_str("\x1b[B");
    if (kDown & KEY_DRIGHT)
      rx_push_str("\x1b[C");
    if (kDown & KEY_DLEFT)
      rx_push_str("\x1b[D");

    // Process touch input for on-screen keyboard
    if (kHeld & KEY_TOUCH) {
      if (!touch_held) {
        touch_held = true;
        touchPosition touch;
        hidTouchRead(&touch);

        // Keyboard: 5 rows starting at pixel y = KB_START_PX_Y
        // Each key: KB_KEY_PX_H pixels tall, KB_KEY_PX_W pixels wide
        int kb_end_y = KB_START_PX_Y + (5 * KB_KEY_PX_H);

        if (touch.py >= KB_START_PX_Y && touch.py < kb_end_y) {
          int r = (touch.py - KB_START_PX_Y) / KB_KEY_PX_H;
          int c = touch.px / KB_KEY_PX_W;
          if (r >= 0 && r < 5 && c >= 0 && c < 10) {
            const char (*current_map)[10] = get_current_keymap();
            char key = current_map[r][c];
            unsigned char uk = (unsigned char)key;

            bool need_redraw = false;

            if (uk == K_SHIFT) {
              // Toggle between lower(0) and upper(1)
              kb_layer = (kb_layer == 1) ? 0 : 1;
              need_redraw = true;
            } else if (uk == K_SYM) {
              kb_layer = 2;
              need_redraw = true;
            } else if (uk == K_ABC) {
              kb_layer = 0;
              need_redraw = true;
            } else if (uk == K_SYM2) {
              kb_layer = 3;
              need_redraw = true;
            } else if (uk == K_CTRL) {
              ctrl_active = !ctrl_active;
              need_redraw = true;
            } else if (uk == (unsigned char)K_TAB) {
              rx_push('\t');
            } else if (uk == K_ESC) {
              rx_push(27);
            } else if (uk == (unsigned char)K_ENTER) {
              rx_push('\n');
            } else if (uk == (unsigned char)K_BKSP) {
              rx_push(127); // DEL (terminal backspace)
            } else if (uk == K_SPACE) {
              rx_push(' ');
            } else {
              // Regular printable character
              if (ctrl_active) {
                // Ctrl+letter -> control character (ASCII 1-26)
                if (key >= 'a' && key <= 'z') {
                  rx_push(key - 'a' + 1);
                } else if (key >= 'A' && key <= 'Z') {
                  rx_push(key - 'A' + 1);
                } else {
                  rx_push(key); // Non-letter with ctrl, just send it
                }
                ctrl_active = false;
                need_redraw = true;
              } else {
                rx_push(key);
                // Auto-unshift after typing in uppercase mode
                if (kb_layer == 1) {
                  kb_layer = 0;
                  need_redraw = true;
                }
              }
            }

            if (need_redraw) {
              draw_keyboard();
            }
          }
        }
      }
    } else {
      touch_held = false;
    }

    // Run Emulator Steps
    uint64_t current_tick = svcGetSystemTick();
    uint32_t elapsedUs = (current_tick - last_tick) / 268; // approx microsecs
    last_tick = current_tick;

    int instrs_per_flip = 10000;
    int ret = MiniRV32IMAStep(core, ram_image, 0, elapsedUs, instrs_per_flip);

    if (ret == 0x5555) { // Poweroff
      break;
    } else if (ret == 3) {
      printf("Emulator fault!\n");
      break;
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  free(ram_image);
  gfxExit();
  return 0;
}
