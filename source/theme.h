#ifndef THEME_H
#define THEME_H

/* The adabit house palette, carried verbatim from adabit.org. The hex values
   are exact: the light column is not a tint of the dark one, each accent is
   deepened until it clears 4.5:1 on the light base.

   "xterm classic" keeps the conventional ANSI 16 that guest programs were
   written against, while still drawing the app's own chrome in house
   colours. */

#include <3ds.h>
#include <ctrosk.h>

typedef struct {
  const char *name;

  /* 0xRRGGBB, no alpha - everything downstream writes a BGR8 framebuffer or
     a TermCell, and neither carries one. */
  u32 base;      /* terminal background, page ground                        */
  u32 surface;   /* cards, the settings panel                               */
  u32 overlay;   /* rows sitting above a card                               */
  u32 muted;     /* secondary labels, "(next launch)" markers               */
  u32 subtle;    /* dividers, disabled text                                 */
  u32 text;      /* terminal foreground, body text                          */
  u32 love;      /* destructive actions, errors                             */
  u32 gold;      /* warnings, values needing attention                      */
  u32 rose;      /* soft accent                                             */
  u32 pine;      /* deep accent                                             */
  u32 foam;      /* informational                                           */
  u32 iris;      /* focus rings and selection - the one interactive colour  */
  u32 hl_low;    /* faintest fill: an unfocused row                         */
  u32 hl_med;    /* default borders                                         */
  u32 hl_high;   /* strongest border                                        */

  /* True when `text` is light on a dark `base`; sets which way ada_lift and
     ada_sink shade. */
  bool dark;
} AdaPalette;

static const AdaPalette ada_dark = {
  .name = "adabit dark",
  .base = 0x0d181f, .surface = 0x1f1d2e, .overlay = 0x26233a,
  .muted = 0x8683a3, .subtle = 0x908caa, .text = 0xe0def4,
  .love = 0xeb6f92, .gold = 0xf6c177, .rose = 0xebbcba,
  .pine = 0x3e8fb0, .foam = 0x9ccfd8, .iris = 0xc4a7e7,
  .hl_low = 0x21202e, .hl_med = 0x403d52, .hl_high = 0x524f67,
  .dark = true,
};

static const AdaPalette ada_light = {
  .name = "adabit light",
  .base = 0xebe0f2, .surface = 0xdbd1e2, .overlay = 0xd2c5dc,
  .muted = 0x594a63, .subtle = 0x4d4056, .text = 0x180b21,
  .love = 0x94435c, .gold = 0x8a5a08, .rose = 0x9d4844,
  .pine = 0x286983, .foam = 0x356871, .iris = 0x6d548a,
  .hl_low = 0xd9d1df, .hl_med = 0xb9adc2, .hl_high = 0xa698b0,
  .dark = false,
};

/* Chrome is the dark house palette; only the ANSI table differs (ada_ansi16). */
static const AdaPalette ada_xterm = {
  .name = "xterm classic",
  .base = 0x0d181f, .surface = 0x1f1d2e, .overlay = 0x26233a,
  .muted = 0x8683a3, .subtle = 0x908caa, .text = 0xe0def4,
  .love = 0xeb6f92, .gold = 0xf6c177, .rose = 0xebbcba,
  .pine = 0x3e8fb0, .foam = 0x9ccfd8, .iris = 0xc4a7e7,
  .hl_low = 0x21202e, .hl_med = 0x403d52, .hl_high = 0x524f67,
  .dark = true,
};

enum { ADA_THEME_DARK = 0, ADA_THEME_LIGHT = 1, ADA_THEME_XTERM = 2, ADA_THEME_COUNT = 3 };

static inline const AdaPalette *ada_palette(int theme) {
  switch (theme) {
    case ADA_THEME_LIGHT: return &ada_light;
    case ADA_THEME_XTERM: return &ada_xterm;
    default:              return &ada_dark;
  }
}

/* ------------------------------------------------------------ colour maths */

static inline u32 ada_rgb(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/* Blend `a` towards `b` by t/256. */
static inline u32 ada_mix(u32 a, u32 b, int t) {
  int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
  int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
  return ada_rgb(ar + ((br - ar) * t >> 8),
                 ag + ((bg - ag) * t >> 8),
                 ab + ((bb - ab) * t >> 8));
}

/* Always toward more contrast against the surface it sits on. */
static inline u32 ada_lift(const AdaPalette *p, u32 c, int t) {
  return ada_mix(c, p->dark ? 0xffffff : 0x000000, t);
}

static inline u32 ada_sink(const AdaPalette *p, u32 c, int t) {
  return ada_mix(c, p->dark ? 0x000000 : 0xffffff, t);
}

/* -------------------------------------------------------------- ANSI table */

/* The conventional xterm 16, kept for ADA_THEME_XTERM. */
static const u32 ada_ansi_xterm[16] = {
  0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
  0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
  0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
  0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
};

/* Fill `out` with the 16 ANSI colours for a theme.

   The palette has no green, so slot 2 borrows foam shifted towards pine.
   Slots 0 and 15 are `base`/`text` so a program painting a black background
   or white text does not punch a hole in the theme. */
static inline void ada_ansi16(const AdaPalette *p, int theme, u32 out[16]) {
  if (theme == ADA_THEME_XTERM) {
    for (int i = 0; i < 16; i++) out[i] = ada_ansi_xterm[i];
    return;
  }

  u32 green = ada_mix(p->foam, p->pine, 96);

  out[0] = p->base;
  out[1] = p->love;
  out[2] = green;
  out[3] = p->gold;
  out[4] = p->pine;
  out[5] = p->iris;
  out[6] = p->foam;
  out[7] = p->subtle;

  for (int i = 0; i < 8; i++) out[8 + i] = ada_lift(p, out[i], 60);
  /* Lifting a near-black by 60/256 does not reach a visible grey. */
  out[8]  = p->muted;
  out[15] = p->text;
}

/* ------------------------------------------------------- keyboard theme --- */

static inline CtrOskColor ada_osk(u32 c) {
  CtrOskColor o = { (u8)((c >> 16) & 0xff), (u8)((c >> 8) & 0xff), (u8)(c & 0xff) };
  return o;
}

/* Build ctrosk's 26-field theme from a palette. The key/mod highlight and
   shadow pairs are computed with ada_lift/ada_sink rather than left to
   ctrosk's additive shade, which does nothing visible on a light face. */
static inline void ada_osk_theme(const AdaPalette *p, CtrOskTheme *t) {
  u32 key = p->overlay;
  u32 mod = ada_mix(p->overlay, p->surface, 128);

  t->bg         = ada_osk(p->base);
  t->status_bar = ada_osk(p->surface);
  t->separator  = ada_osk(p->hl_med);
  t->badge      = ada_osk(p->hl_low);

  t->key    = ada_osk(key);
  t->key_hi = ada_osk(ada_lift(p, key, 36));
  t->key_sh = ada_osk(ada_sink(p, key, 36));
  t->num    = ada_osk(ada_sink(p, key, 24));
  t->mod    = ada_osk(mod);
  t->mod_hi = ada_osk(ada_lift(p, mod, 36));
  t->mod_sh = ada_osk(ada_sink(p, mod, 36));

  /* Accents are sunk hard towards the surface: a full-strength one as a key
     face is too loud, and the label on top has to stay readable. */
  t->shift_on = ada_osk(ada_sink(p, p->foam, 140));
  t->ctrl_on  = ada_osk(ada_sink(p, p->love, 140));
  t->sym      = ada_osk(ada_sink(p, p->gold, 150));
  t->enter    = ada_osk(ada_sink(p, p->pine, 130));
  t->del      = ada_osk(ada_sink(p, p->love, 150));
  t->space    = ada_osk(ada_mix(key, p->surface, 64));

  t->text     = ada_osk(p->text);
  t->text_num = ada_osk(p->gold);
  t->text_dim = ada_osk(p->muted);

  t->label_tab     = ada_osk(p->foam);
  t->label_esc     = ada_osk(p->iris);
  /* pine is the darkest accent in both columns; unlifted it is unreadable on
     a dark key face. */
  t->label_abc     = ada_osk(ada_lift(p, p->pine, 60));
  t->label_sym     = ada_osk(p->gold);
  t->label_enter   = ada_osk(ada_lift(p, p->pine, 90));
  t->label_del     = ada_osk(ada_lift(p, p->love, 90));
  t->label_ctrl_on = ada_osk(ada_lift(p, p->love, 120));
}

#endif /* THEME_H */
