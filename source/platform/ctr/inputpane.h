#ifndef INPUTPANE_H
#define INPUTPANE_H

/* The bottom screen when the keyboard setting is "3DS system", instead of the
 * realtime touch keyboard from vendor/ctr-osk-rt.
 *
 * swkbdInputText is a *modal applet*: it takes both screens, blocks until the
 * user confirms or cancels, and Horizon suspends this process for the
 * duration. There is no per-keystroke callback, so this is a line-compose
 * mode rather than a drop-in replacement - hence the raw-key strip, which
 * sends Ctrl-C, Tab, Esc and the arrows immediately with no applet involved,
 * so anything interactive stays controllable.
 *
 * Include after term_state, ui_lock, g_osk and rx_push_str exist.
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "config.h"
#include "theme.h"
#include "ui.h"

#define PANE_TYPE_X   16
#define PANE_TYPE_Y   72
#define PANE_TYPE_W  288
#define PANE_TYPE_H   40

#define PANE_KEY_Y   150
#define PANE_KEY_H    34
#define PANE_KEY_N     8

static bool pane_dirty = true;
static char pane_last[64];     /* echo of the last line sent                 */
static char pane_buf[256];     /* swkbd's output, kept as its initial text    */

/* The raw keys, in draw order. `send` goes straight into the guest's input
   ring. */
static const struct { const char *label; const char *send; } pane_keys[PANE_KEY_N] = {
  { "^C",  "\x03"     },
  { "^D",  "\x04"     },
  { "^Z",  "\x1a"     },
  { "TAB", "\t"       },
  { "ESC", "\x1b"     },
  { "UP",  "\x1b[A"   },
  { "DN",  "\x1b[B"   },
  { "RET", "\r"       },
};

/* Open the system keyboard and send whatever comes back. Must not be called
 * from inside a frame. */
static void pane_prompt(void) {
  SwkbdState kb;
  swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof(pane_buf) - 1);
  swkbdSetHintText(&kb, "Command to send to the guest");
  swkbdSetFeatures(&kb, SWKBD_MULTILINE);
  /* Seed with the previous line: re-running something with one character
     changed is the common case at a shell. */
  if (pane_last[0]) swkbdSetInitialText(&kb, pane_last);

  SwkbdButton btn = swkbdInputText(&kb, pane_buf, sizeof(pane_buf));

  /* Everything below is recovery from having been suspended by the applet. */

  /* 1. The guest's clock. EmuStepBatch would otherwise hand the guest the
        whole length of the typing session in one step, firing a backlog of
        timer interrupts and an RCU stall splat. */
  g_emu_rebase_clock = true;

  /* 2. Both framebuffers: the applet drew over them, and neither redraw path
        repaints unless it thinks something changed. */
  term_state.dirty = true;
  pane_dirty = true;

  /* 3. Double buffering on the bottom screen. ctrOskInit turned it off, and
        swkbd is a separate process that has been driving this screen since.
        Costs nothing if it survived. */
  gfxSetDoubleBuffering(GFX_BOTTOM, false);

  if (btn != SWKBD_BUTTON_CONFIRM) return;

  rx_push_str(pane_buf);
  rx_push('\n');

  /* Bounded explicitly rather than letting snprintf truncate, so the compiler
     can see the truncation is intended. */
  snprintf(pane_last, sizeof(pane_last), "%.*s",
           (int)sizeof(pane_last) - 1, pane_buf);
}

/* Hit test and dispatch. Returns true if the touch was consumed. */
static bool pane_touch(const plat_input_t *in) {
  int px = in->ptr_x, py = in->ptr_y;
  if (px >= PANE_TYPE_X && px < PANE_TYPE_X + PANE_TYPE_W &&
      py >= PANE_TYPE_Y && py < PANE_TYPE_Y + PANE_TYPE_H) {
    pane_prompt();
    return true;
  }

  if (py >= PANE_KEY_Y && py < PANE_KEY_Y + PANE_KEY_H) {
    int kw = UI_W / PANE_KEY_N;
    int i = px / kw;
    if (i >= 0 && i < PANE_KEY_N) {
      rx_push_str(pane_keys[i].send);
      pane_dirty = true;
      return true;
    }
  }
  return false;
}

static void pane_draw(void) {
  if (!pane_dirty) return;
  pane_dirty = false;

  const AdaPalette *p = ada_palette(g_cfg.theme);

  ui_bind();
  ui_clear(p->base);

  /* Status bar, matching the realtime keyboard's. */
  ui_fill(0, 0, UI_W, 38, p->surface);
  ui_hline(0, 37, UI_W, p->hl_med);
  ui_text(8, 15, "3ds-cli", p->text, 1);
  ui_text_right(UI_W - 8, 15, "adabit.org", p->muted, 1);

  ui_card(p, 8, 48, UI_W - 16, 78);
  ui_text(16, 56, "Line input", p->muted, 1);

  ui_button(p, PANE_TYPE_X, PANE_TYPE_Y, PANE_TYPE_W, PANE_TYPE_H,
            "Type a command...", UI_BTN_SELECTED);

  if (pane_last[0]) {
    char echo[42];
    snprintf(echo, sizeof(echo), "sent: %.*s", (int)sizeof(echo) - 7, pane_last);
    ui_text_clip(16, 132, UI_W - 32, echo, p->foam, 1);
  } else {
    ui_text(16, 132, "A or tap to open the 3DS keyboard", p->muted, 1);
  }

  int kw = UI_W / PANE_KEY_N;
  for (int i = 0; i < PANE_KEY_N; i++) {
    ui_button(p, i * kw + 2, PANE_KEY_Y, kw - 4, PANE_KEY_H,
              pane_keys[i].label, UI_BTN_NORMAL);
  }

  ui_text_centre(0, PANE_KEY_Y + PANE_KEY_H + 12, UI_W,
                 "these send at once, no applet", p->muted, 1);
  ui_text_centre(0, PANE_KEY_Y + PANE_KEY_H + 26, UI_W,
                 "SELECT settings   START quit", p->muted, 1);
}

#endif /* INPUTPANE_H */
