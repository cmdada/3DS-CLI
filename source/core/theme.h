#ifndef THEME_H
#define THEME_H

/* The adabit house palette, carried verbatim from adabit.org. The hex values
   are exact: the light column is not a tint of the dark one, each accent is
   deepened until it clears 4.5:1 on the light base.

   "xterm classic" keeps the conventional ANSI 16 that guest programs were
   written against, while still drawing the app's own chrome in house
   colours. */

#include <stdint.h>

typedef struct AdaPalette {
  const char *name;

  /* 0xRRGGBB, no alpha - everything downstream writes a BGR8 framebuffer or
     a TermCell, and neither carries one. */
  uint32_t base;      /* terminal background, page ground                        */
  uint32_t surface;   /* cards, the settings panel                               */
  uint32_t overlay;   /* rows sitting above a card                               */
  uint32_t muted;     /* secondary labels, "(next launch)" markers               */
  uint32_t subtle;    /* dividers, disabled text                                 */
  uint32_t text;      /* terminal foreground, body text                          */
  uint32_t love;      /* destructive actions, errors                             */
  uint32_t gold;      /* warnings, values needing attention                      */
  uint32_t rose;      /* soft accent                                             */
  uint32_t pine;      /* deep accent                                             */
  uint32_t foam;      /* informational                                           */
  uint32_t iris;      /* focus rings and selection - the one interactive colour  */
  uint32_t hl_low;    /* faintest fill: an unfocused row                         */
  uint32_t hl_med;    /* default borders                                         */
  uint32_t hl_high;   /* strongest border                                        */

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

static inline uint32_t ada_rgb(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Blend `a` towards `b` by t/256. */
static inline uint32_t ada_mix(uint32_t a, uint32_t b, int t) {
  int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
  int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
  return ada_rgb(ar + ((br - ar) * t >> 8),
                 ag + ((bg - ag) * t >> 8),
                 ab + ((bb - ab) * t >> 8));
}

/* Always toward more contrast against the surface it sits on. */
static inline uint32_t ada_lift(const AdaPalette *p, uint32_t c, int t) {
  return ada_mix(c, p->dark ? 0xffffff : 0x000000, t);
}

static inline uint32_t ada_sink(const AdaPalette *p, uint32_t c, int t) {
  return ada_mix(c, p->dark ? 0x000000 : 0xffffff, t);
}

/* -------------------------------------------------------------- ANSI table */

/* The conventional xterm 16, kept for ADA_THEME_XTERM. */
static const uint32_t ada_ansi_xterm[16] = {
  0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
  0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
  0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
  0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
};

/* Fill `out` with the 16 ANSI colours for a theme.

   The palette has no green, so slot 2 borrows foam shifted towards pine.
   Slots 0 and 15 are `base`/`text` so a program painting a black background
   or white text does not punch a hole in the theme. */
static inline void ada_ansi16(const AdaPalette *p, int theme, uint32_t out[16]) {
  if (theme == ADA_THEME_XTERM) {
    for (int i = 0; i < 16; i++) out[i] = ada_ansi_xterm[i];
    return;
  }

  uint32_t green = ada_mix(p->foam, p->pine, 96);

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

#endif /* THEME_H */
