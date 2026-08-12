#ifndef SETTINGS_H
#define SETTINGS_H

/* The settings page, on the bottom screen.
 *
 * Include from main.c *after* term_state, ui_lock, g_osk, rx_push_str and the
 * virtio device headers exist - it reads all of them.
 *
 * Rules this file lives under (threading):
 *
 * - Everything here runs on the main thread. It may write term_state's
 *   main-thread-owned fields (zoom_*, scroll_*, auto_track, use_5x7,
 *   cursor_blink) with no lock, and MUST hold ui_lock to touch default_fg,
 *   default_bg, the grid or the scrollback - which is what a theme change
 *   does.
 *
 * - The bottom screen is single-buffered and ctrOskDraw() clears all of it, so
 *   the keyboard and this page cannot both be up. While this page is open
 *   main.c does not call ctrOskDraw at all; closing it calls ctrOskInvalidate
 *   so the keyboard paints itself back.
 *
 * - Drawing is gated on settings_dirty: repainting 320x240 by hand every tick
 *   comes straight out of guest throughput.
 *
 * Device rows do nothing until the next launch. main.c snapshots the dev_*
 * flags before starting the emulation thread and the MMIO decode reads the
 * snapshot, never g_cfg.
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "config.h"
#include "theme.h"
#include "ui3ds.h"

/* ------------------------------------------------------------------ layout */

#define SET_TAB_H     26
#define SET_CARD_X     6
#define SET_CARD_Y    30
#define SET_CARD_W   308
#define SET_CARD_H   178
#define SET_ROW_X     14
#define SET_ROW_W    286
#define SET_ROW_Y0    38
#define SET_ROW_H     26
#define SET_ROWS_VIS   6
#define SET_FOOT_Y   214

/* ------------------------------------------------------------- row model -- */

typedef enum {
  ROW_TOGGLE,     /* bool field, drawn as a switch                          */
  ROW_BOOLCHOICE, /* bool field, drawn as one of two words                  */
  ROW_CHOICE,     /* int field, values[] with matching labels[]             */
  ROW_ACTION,     /* runs something; no stored state                        */
  ROW_INFO,       /* read-only; not focusable                               */
} RowKind;

enum {
  ACT_RESET_ROOTFS = 1,
  ACT_RESTORE_DEFAULTS,
  ACT_SAVE_QUIT,
};

enum {
  INFO_MODEL = 1,
  INFO_RAM,
  INFO_DEVICES,
};

typedef struct {
  const char        *label;
  RowKind            kind;
  size_t             off;      /* into Cfg, for the three stateful kinds    */
  const char *const *labels;
  const int         *values;
  int                n;
  bool               next_launch;
  int                id;       /* ACT_* or INFO_*                           */
} Row;

static const char *const lbl_theme[]  = { "adabit dark", "adabit light", "xterm classic" };
static const int         val_theme[]  = { ADA_THEME_DARK, ADA_THEME_LIGHT, ADA_THEME_XTERM };
static const char *const lbl_font[]   = { "8x8", "5x7" };
static const char *const lbl_bg[]     = { "theme", "black" };
static const char *const lbl_kbd[]    = { "realtime", "3DS system" };
static const int         val_kbd[]    = { 0, 1 };
static const char *const lbl_bksp[]   = { "BS (8)", "DEL (127)" };
static const char *const lbl_circle[] = { "arrows", "pan view" };
static const char *const lbl_zoom[]   = { "1x", "2x", "3x", "4x", "5x" };
static const int         val_zoom[]   = { 1, 2, 3, 4, 5 };
static const char *const lbl_ram[]    = { "auto", "16 MB", "24 MB", "32 MB", "48 MB", "64 MB" };
static const int         val_ram[]    = { 0, 16, 24, 32, 48, 64 };

#define R_TOGGLE(lab, f, nl)      { lab, ROW_TOGGLE, offsetof(Cfg, f), NULL, NULL, 0, nl, 0 }
#define R_BOOL(lab, f, l, nl)     { lab, ROW_BOOLCHOICE, offsetof(Cfg, f), l, NULL, 2, nl, 0 }
#define R_CHOICE(lab, f, l, v, nl) { lab, ROW_CHOICE, offsetof(Cfg, f), l, v, \
                                     (int)(sizeof(v) / sizeof((v)[0])), nl, 0 }
#define R_ACTION(lab, act)        { lab, ROW_ACTION, 0, NULL, NULL, 0, false, act }
#define R_INFO(lab, inf)          { lab, ROW_INFO, 0, NULL, NULL, 0, false, inf }

static const Row rows_display[] = {
  R_CHOICE("Theme",             theme,        lbl_theme, val_theme, false),
  R_BOOL  ("Top background",    top_black,    lbl_bg,               false),
  R_BOOL  ("Font",              use_5x7,      lbl_font,             false),
  R_CHOICE("Zoom horizontal",   zoom_x,       lbl_zoom,  val_zoom,  false),
  R_CHOICE("Zoom vertical",     zoom_y,       lbl_zoom,  val_zoom,  false),
  R_TOGGLE("Cursor blink",      cursor_blink,                       false),
  R_TOGGLE("Follow output",     follow_output,                      false),
};

static const Row rows_input[] = {
  R_CHOICE("Keyboard",          keyboard,     lbl_kbd,   val_kbd,   false),
  R_BOOL  ("Backspace sends",   backspace_del, lbl_bksp,            false),
  R_TOGGLE("Shift is one-shot", shift_oneshot,                      false),
  R_BOOL  ("Circle pad",        circle_pans,  lbl_circle,           false),
};

static const Row rows_hw[] = {
  R_TOGGLE("Network (NAT)",     dev_net,      true),
  R_TOGGLE("SD passthrough",    dev_sd,       true),
  R_TOGGLE("NAND passthrough",  dev_nand,     true),
  R_TOGGLE("TWL passthrough",   dev_twl,      true),
  R_TOGGLE("Sensors",           dev_sensors,  true),
  R_TOGGLE("Hardware RNG",      dev_rng,      true),
  R_TOGGLE("Swap file",         dev_swap,     true),
  R_CHOICE("Guest RAM cap",     ram_cap_mb,   lbl_ram,   val_ram,   true),
};

/* Exactly SET_ROWS_VIS rows, deliberately: the three info rows are not
   focusable, so a longer section could never scroll them into view. */
static const Row rows_system[] = {
  R_ACTION("Reset guest state from Image", ACT_RESET_ROOTFS),
  R_ACTION("Restore default settings",     ACT_RESTORE_DEFAULTS),
  R_ACTION("Save & quit",                  ACT_SAVE_QUIT),
  R_INFO  ("Model",                        INFO_MODEL),
  R_INFO  ("Guest RAM",                    INFO_RAM),
  R_INFO  ("Devices up",                   INFO_DEVICES),
};

typedef struct { const char *name; const Row *rows; int n; } Section;

static const Section sections[] = {
  { "Display",  rows_display, (int)(sizeof(rows_display) / sizeof(Row)) },
  { "Input",    rows_input,   (int)(sizeof(rows_input)   / sizeof(Row)) },
  { "Hardware", rows_hw,      (int)(sizeof(rows_hw)      / sizeof(Row)) },
  { "System",   rows_system,  (int)(sizeof(rows_system)  / sizeof(Row)) },
};
#define SET_NSECTIONS ((int)(sizeof(sections) / sizeof(sections[0])))

/* ------------------------------------------------------------------ state - */

static bool settings_open  = false;
static bool settings_dirty = true;
static int  settings_sec   = 0;
static int  settings_sel   = 0;
static int  settings_top   = 0;   /* first visible row                       */
static int  settings_confirm = 0; /* ACT_* awaiting a yes/no                 */
static bool settings_quit  = false;
static char settings_toast[40];   /* one-line result of the last action      */

static CtrOskTheme g_osk_theme;

/* Set by main() once the Image has been sized. Without a bundled rootfs there
   is nothing to re-extract, and resetting would delete the only copy. */
static bool g_have_bundle = false;

/* ---------------------------------------------------------------- helpers - */

static inline bool row_focusable(const Row *r) {
  if (r->kind == ROW_INFO) return false;
  if (r->id == ACT_RESET_ROOTFS && !g_have_bundle) return false;
  return true;
}

static inline bool row_disabled(const Row *r) {
  return r->id == ACT_RESET_ROOTFS && !g_have_bundle;
}

/* Rows carry their own offset into Cfg; `off` does not sit at the same place
   in Row as it does in CfgField, so config.h's accessors do not apply. */
static inline bool *row_boolp(Cfg *c, const Row *r) {
  return (bool *)((char *)c + r->off);
}
static inline int *row_intp(Cfg *c, const Row *r) {
  return (int *)((char *)c + r->off);
}

/* Index of the stored value in values[], or 0 if it is not one of them - which
   only a hand-edited config file can produce. */
static inline int row_choice_index(const Cfg *c, const Row *r) {
  int v = *row_intp((Cfg *)c, r);
  for (int i = 0; i < r->n; i++) if (r->values[i] == v) return i;
  return 0;
}

static void settings_apply_live(void);

/* Cycle a stateful row by `dir` (+1 / -1), or flip it for A. */
static void row_cycle(Cfg *c, const Row *r, int dir) {
  switch (r->kind) {
    case ROW_TOGGLE:
    case ROW_BOOLCHOICE: {
      bool *b = row_boolp(c, r);
      *b = !*b;
      break;
    }
    case ROW_CHOICE: {
      int i = row_choice_index(c, r) + dir;
      if (i < 0) i = r->n - 1;
      if (i >= r->n) i = 0;
      *row_intp(c, r) = r->values[i];
      break;
    }
    default: break;
  }
}

/* ------------------------------------------------------------ value text -- */

static void settings_info_text(int id, char *out, size_t n);

static void row_value_text(const Cfg *c, const Row *r, char *out, size_t n) {
  out[0] = '\0';
  switch (r->kind) {
    case ROW_BOOLCHOICE:
      snprintf(out, n, "%s", r->labels[*row_boolp((Cfg *)c, r) ? 1 : 0]);
      break;
    case ROW_CHOICE:
      snprintf(out, n, "%s", r->labels[row_choice_index(c, r)]);
      break;
    case ROW_INFO:
      settings_info_text(r->id, out, n);
      break;
    case ROW_ACTION:
      if (row_disabled(r)) snprintf(out, n, "no rootfs in Image");
      break;
    default: break; /* ROW_TOGGLE draws a switch, not text */
  }
}

/* ------------------------------------------------------------------ input - */

static void settings_move(int dir) {
  const Section *s = &sections[settings_sec];
  int i = settings_sel;
  for (int guard = 0; guard < s->n; guard++) {
    i += dir;
    if (i < 0 || i >= s->n) return;           /* stop at the ends, no wrap */
    if (row_focusable(&s->rows[i])) { settings_sel = i; break; }
  }
  if (settings_sel < settings_top) settings_top = settings_sel;
  if (settings_sel >= settings_top + SET_ROWS_VIS)
    settings_top = settings_sel - SET_ROWS_VIS + 1;
}

static void settings_set_section(int sec) {
  if (sec < 0) sec = SET_NSECTIONS - 1;
  if (sec >= SET_NSECTIONS) sec = 0;
  settings_sec = sec;
  settings_top = 0;
  settings_sel = 0;
  const Section *s = &sections[sec];
  while (settings_sel < s->n && !row_focusable(&s->rows[settings_sel])) settings_sel++;
  if (settings_sel >= s->n) settings_sel = 0;
  settings_toast[0] = '\0';
}

static void settings_run_action(int act);

/* Returns false when the page has just closed. */
static bool settings_update(u32 kDown, const touchPosition *touch, bool touched) {
  const Section *s = &sections[settings_sec];

  /* Confirmations are modal: nothing else on the page responds while one is
     up. */
  if (settings_confirm) {
    if (kDown & KEY_A) { settings_run_action(settings_confirm); settings_confirm = 0; }
    else if (kDown & (KEY_B | KEY_SELECT)) { settings_confirm = 0; }
    else return true;
    settings_dirty = true;
    return true;
  }

  if (kDown & (KEY_B | KEY_SELECT)) { settings_open = false; return false; }

  if (kDown & KEY_L) { settings_set_section(settings_sec - 1); settings_dirty = true; }
  if (kDown & KEY_R) { settings_set_section(settings_sec + 1); settings_dirty = true; }
  if (kDown & KEY_DUP)   { settings_move(-1); settings_dirty = true; }
  if (kDown & KEY_DDOWN) { settings_move(+1); settings_dirty = true; }

  /* A tap selects the row under the finger and, for a stateful row, also
     activates it - one gesture, not two. Rows are 26px tall because a finger
     lands only near where it was aimed. */
  if (touched && touch->py >= SET_ROW_Y0 &&
      touch->py < SET_ROW_Y0 + SET_ROWS_VIS * SET_ROW_H &&
      touch->px >= SET_ROW_X && touch->px < SET_ROW_X + SET_ROW_W) {
    int idx = settings_top + (touch->py - SET_ROW_Y0) / SET_ROW_H;
    if (idx < s->n && row_focusable(&s->rows[idx])) {
      settings_sel = idx;
      const Row *r = &s->rows[idx];
      if (r->kind == ROW_ACTION) {
        if (r->id == ACT_RESET_ROOTFS) settings_confirm = r->id;
        else settings_run_action(r->id);
      } else {
        row_cycle(&g_cfg, r, +1);
        settings_apply_live();
      }
      settings_dirty = true;
    }
  }

  if (settings_sel >= s->n) return true;
  const Row *r = &s->rows[settings_sel];

  if (kDown & (KEY_A | KEY_DRIGHT | KEY_DLEFT)) {
    if (r->kind == ROW_ACTION) {
      if (kDown & KEY_A) {
        /* The one action here that cannot be undone by flipping a row back. */
        if (r->id == ACT_RESET_ROOTFS) settings_confirm = r->id;
        else settings_run_action(r->id);
      }
    } else {
      row_cycle(&g_cfg, r, (kDown & KEY_DLEFT) ? -1 : +1);
      settings_apply_live();
    }
    settings_dirty = true;
  }

  return true;
}

/* ------------------------------------------------------------------- draw - */

static void settings_draw(void) {
  if (!settings_dirty) return;
  settings_dirty = false;

  const AdaPalette *p = ada_palette(g_cfg.theme);
  const Section *s = &sections[settings_sec];

  ui_bind();
  ui_clear(p->base);

  const char *names[SET_NSECTIONS];
  for (int i = 0; i < SET_NSECTIONS; i++) names[i] = sections[i].name;
  ui_tabbar(p, 0, 0, UI_W, SET_TAB_H, names, SET_NSECTIONS, settings_sec);

  ui_card(p, SET_CARD_X, SET_CARD_Y, SET_CARD_W, SET_CARD_H);

  for (int i = 0; i < SET_ROWS_VIS; i++) {
    int idx = settings_top + i;
    if (idx >= s->n) break;
    const Row *r = &s->rows[idx];
    int y = SET_ROW_Y0 + i * SET_ROW_H;
    bool focused = (idx == settings_sel);
    bool dis = row_disabled(r);

    char val[40];
    row_value_text(&g_cfg, r, val, sizeof(val));

    /* A toggle draws its own switch, so the row is told it has no value text
       and the switch goes into the gap afterwards. */
    if (r->kind == ROW_TOGGLE) {
      ui_row(p, SET_ROW_X, y, SET_ROW_W, SET_ROW_H, r->label, NULL, focused, dis);
      ui_toggle(p, SET_ROW_X + SET_ROW_W - UI_TOGGLE_W - 8,
                y + (SET_ROW_H - UI_TOGGLE_H) / 2, *row_boolp(&g_cfg, r));
    } else {
      ui_row(p, SET_ROW_X, y, SET_ROW_W, SET_ROW_H, r->label,
             val[0] ? val : NULL, focused, dis);
    }

    /* Say so under the label, or a toggle that visibly flips but changes
       nothing reads as broken. */
    if (r->next_launch) {
      ui_text(SET_ROW_X + 8, y + SET_ROW_H - 9, "next launch", p->muted, 1);
    }

    if (focused) ui_focus_ring(p, SET_ROW_X, y, SET_ROW_W, SET_ROW_H, UI_R_ROW);
  }

  ui_scrollbar(p, SET_CARD_X + SET_CARD_W - 7, SET_ROW_Y0,
               SET_ROWS_VIS * SET_ROW_H, settings_top, s->n, SET_ROWS_VIS);

  if (settings_toast[0]) {
    ui_text_centre(0, SET_FOOT_Y + 6, UI_W, settings_toast, p->foam, 1);
  } else {
    ui_text_centre(0, SET_FOOT_Y, UI_W, "L/R section   D-pad move   A change", p->muted, 1);
    ui_text_centre(0, SET_FOOT_Y + 12, UI_W, "B close   START quit", p->muted, 1);
  }

  if (settings_confirm) {
    int cw = 260, ch = 96, cx = (UI_W - cw) / 2, cy = (UI_H - ch) / 2;
    ui_rrect(cx, cy, cw, ch, UI_R_CARD, p->overlay);
    ui_rrect_outline(cx, cy, cw, ch, UI_R_CARD, p->love);
    ui_text_centre(cx, cy + 14, cw, "Reset guest state?", p->text, 1);
    ui_text_centre(cx, cy + 32, cw, "Deletes rootfs.ext2 and re-extracts", p->muted, 1);
    ui_text_centre(cx, cy + 42, cw, "it from Image at the next launch.", p->muted, 1);
    ui_text_centre(cx, cy + 52, cw, "Everything in the guest is lost.", p->muted, 1);
    ui_button(p, cx + 20, cy + ch - 30, 100, 22, "A  reset", UI_BTN_DANGER);
    ui_button(p, cx + cw - 120, cy + ch - 30, 100, 22, "B  cancel", UI_BTN_NORMAL);
  }
}

/* ------------------------------------------------- apply, actions, info --- */
/* Everything below reaches into main.c's globals - term_state, ui_lock, g_osk
   and the virtio devices. */

static void settings_apply_live(void) {
  const AdaPalette *p = ada_palette(g_cfg.theme);

  u32 pal[16];
  ada_ansi16(p, g_cfg.theme, pal);
  u32 fg = p->text;
  /* A true 0x000000, not a near-black theme colour - the contrast is the whole
     point of the option. */
  u32 bg = g_cfg.top_black ? 0x000000 : p->base;

  /* The one part of a settings change that touches emu-thread-owned state. */
  LightLock_Lock(&ui_lock);
  term_set_palette(&term_state, pal, fg, bg);
  LightLock_Unlock(&ui_lock);

  term_state.use_5x7      = g_cfg.use_5x7;
  term_state.zoom_x       = g_cfg.zoom_x;
  term_state.zoom_y       = g_cfg.zoom_y;
  term_state.cursor_blink = g_cfg.cursor_blink;
  if (g_cfg.follow_output && !term_state.auto_track) term_state.scroll_y = 0;
  term_state.auto_track   = g_cfg.follow_output;
  term_state.dirty        = true;

  ada_osk_theme(p, &g_osk_theme);
  g_osk.theme            = &g_osk_theme;
  g_osk.backspace_as_del = g_cfg.backspace_del;
  g_osk.shift_is_oneshot = g_cfg.shift_oneshot;
  ctrOskInvalidate(&g_osk);

  settings_dirty = true;
}

static void settings_run_action(int act) {
  switch (act) {
    case ACT_RESET_ROOTFS:
      /* A request, not the deed: the emulation thread has rootfs.ext2 open.
         main() consumes this flag at the next launch. */
      g_cfg.reset_rootfs = true;
      cfg_save(&g_cfg);
      snprintf(settings_toast, sizeof(settings_toast), "Guest resets at next launch");
      break;

    case ACT_RESTORE_DEFAULTS: {
      /* reset_rootfs is a pending request, not a preference - clearing it
         would cancel a reset the user already confirmed. */
      bool pending = g_cfg.reset_rootfs;
      cfg_defaults(&g_cfg);
      g_cfg.reset_rootfs = pending;
      settings_apply_live();
      cfg_save(&g_cfg);
      snprintf(settings_toast, sizeof(settings_toast), "Defaults restored");
      break;
    }

    case ACT_SAVE_QUIT:
      cfg_save(&g_cfg);
      settings_quit = true;
      break;
  }
  settings_dirty = true;
}

static void settings_info_text(int id, char *out, size_t n) {
  switch (id) {
    case INFO_MODEL:
      snprintf(out, n, "%s", g_new_3ds ? "New3DS" : "Old3DS");
      break;
    case INFO_RAM:
      snprintf(out, n, "%lu MB", (unsigned long)(ram_amt >> 20));
      break;
    case INFO_DEVICES: {
      /* What actually came up this launch, not what the config asked for. */
      char b[40];
      b[0] = '\0';
      if (vnet.soc_ready)               strncat(b, "net ",  sizeof(b) - strlen(b) - 1);
      if (v9p_tree_ok[V9P_TREE_NAND])   strncat(b, "nand ", sizeof(b) - strlen(b) - 1);
      if (v9p_tree_ok[V9P_TREE_TWL])    strncat(b, "twl ",  sizeof(b) - strlen(b) - 1);
      if (hw.sensors)                   strncat(b, "sens ", sizeof(b) - strlen(b) - 1);
      if (vrng.ps_ready)                strncat(b, "rng",   sizeof(b) - strlen(b) - 1);
      snprintf(out, n, "%s", b[0] ? b : "none");
      break;
    }
    default: out[0] = '\0'; break;
  }
}

/* ----------------------------------------------------------- open / close - */

static void settings_enter(void) {
  settings_open    = true;
  settings_confirm = 0;
  settings_quit    = false;
  settings_toast[0] = '\0';
  settings_set_section(settings_sec);   /* keep the tab, re-seat the cursor */
  settings_dirty   = true;
}

/* Hands the bottom screen back. ctrOskDraw is a no-op unless it thinks
   something changed, and it has no idea this page painted over it. */
static void settings_leave(void) {
  settings_open = false;
  cfg_save(&g_cfg);
  ctrOskInvalidate(&g_osk);
}

#endif /* SETTINGS_H */
