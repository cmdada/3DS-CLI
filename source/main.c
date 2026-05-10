#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PrintConsole topScreen, bottomScreen;

const char keymap_lower[4][10] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-'},
    {'^', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.'},
    {' ', ' ', ' ', ' ', '\n', '\n', '\n', '\b', '\b', '\b'}};

const char keymap_upper[4][10] = {
    {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
    {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '_'},
    {'^', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>'},
    {' ', ' ', ' ', ' ', '\n', '\n', '\n', '\b', '\b', '\b'}};

bool shifted = false;

void draw_keyboard() {
  consoleSelect(&bottomScreen);
  printf("\x1b[2J"); // Clear screen

  printf("\x1b[1;1H");
  printf("3ds-cli by cmdada (https://adabit.org)\n");
  printf("running mini-rv32ima image,login as root\n");
  printf("Tap the bottom screen to type.\n");
  printf("Press START to exit.\n\n");

  const char (*current_map)[10] = shifted ? keymap_upper : keymap_lower;

  // We will draw keys at specific text row/cols.
  // Top-left of keyboard is at text row 13 (pixel y=104).
  // Each key is 4 chars wide (32 pixels) and 4 text rows tall (32 pixels).
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 10; c++) {
      char key = current_map[r][c];
      int text_r = 13 + (r * 4);
      int text_c = c * 4;

      char display_char = key;
      if (key == '^')
        display_char = '^'; // Shift

      if (r == 0) {
        printf("\x1b[%d;%dH+---+", text_r, text_c + 1);
      }
      printf("\x1b[%d;%dH|   |", text_r + 1, text_c + 1);
      printf("\x1b[%d;%dH| %c |", text_r + 2, text_c + 1, display_char);
      printf("\x1b[%d;%dH|   |", text_r + 3, text_c + 1);
      printf("\x1b[%d;%dH+---+", text_r + 4, text_c + 1);
    }
  }

  // Draw row 3 manually for large special keys
  int text_r = 13 + (3 * 4);

  // SPACE: cols 0..3 (16 chars wide, starts at 1)
  printf("\x1b[%d;1H|               |", text_r + 1);
  printf("\x1b[%d;1H|     SPACE     |", text_r + 2);
  printf("\x1b[%d;1H|               |", text_r + 3);
  printf("\x1b[%d;1H+---------------+", text_r + 4);

  // ENTER: cols 4..6 (12 chars wide, starts at 17)
  printf("\x1b[%d;17H|           |", text_r + 1);
  printf("\x1b[%d;17H|   ENTER   |", text_r + 2);
  printf("\x1b[%d;17H|           |", text_r + 3);
  printf("\x1b[%d;17H+-----------+", text_r + 4);

  // BACKSPACE: cols 7..9 (12 chars wide, starts at 29)
  printf("\x1b[%d;29H|           |", text_r + 1);
  printf("\x1b[%d;29H|   BKSPC   |", text_r + 2);
  printf("\x1b[%d;29H|           |", text_r + 3);
  printf("\x1b[%d;29H+-----------+", text_r + 4);
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

    // Process touch input
    if (kHeld & KEY_TOUCH) {
      if (!touch_held) {
        touch_held = true;
        touchPosition touch;
        hidTouchRead(&touch);

        if (touch.py >= 104 && touch.py < 232) {
          int r = (touch.py - 104) / 32;
          int c = touch.px / 32;
          if (r >= 0 && r < 4 && c >= 0 && c < 10) {
            const char (*current_map)[10] =
                shifted ? keymap_upper : keymap_lower;
            char key = current_map[r][c];

            if (key == '^') {
              shifted = !shifted;
              draw_keyboard();
            } else {
              int next_head = (rx_head + 1) % 256;
              if (next_head != rx_tail) {
                rx_buf[rx_head] = key;
                rx_head = next_head;
              }
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
