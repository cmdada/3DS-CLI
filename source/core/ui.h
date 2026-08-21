#ifndef CORE_UI_H
#define CORE_UI_H

/* Panel drawing, in the panorama_3ds house style, straight against the raw
 * framebuffer - there is no C3D/C2D context in this app.
 *
 * Every primitive is built from ui_vspan, a vertical run at fixed x. That is
 * the shape the 3DS's rotated framebuffer is laid out for, where a column is
 * contiguous and a row is not; on a console with a linear surface the two
 * simply swap and the run is strided instead. The panel is dirty-tracked and
 * repaints rarely, so that costs little and keeps one set of primitives.
 *
 * Where the panel sits on the physical screen, and whether it needs flipping
 * at all, is the backend's business - see plat_surface/plat_present.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "plat.h"
#include "font.h"
#include "theme.h"

#define UI_W PLAT_PANEL_W
#define UI_H PLAT_PANEL_H

/* 3DS-scale equivalents of the style guide's --radius-lg and --radius-md. */
#define UI_R_CARD 6
#define UI_R_ROW  4

static plat_fb_t ui_s;
static bool      ui_bound = false;

/* Call once per redraw, before anything else - every primitive below assumes
   it has been called. */
static inline void ui_bind(void) {
  ui_bound = plat_surface(PLAT_SURF_PANEL, &ui_s);
}

static inline void ui_px(int x, int y, uint32_t c) {
  if (!ui_bound || x < 0 || x >= UI_W || y < 0 || y >= UI_H) return;
  uint8_t *o = ui_s.base + (ptrdiff_t)x * ui_s.x_stride
                         + (ptrdiff_t)y * ui_s.y_stride;
  PLAT_PX(o, (uint8_t)((c >> 16) & 0xff), (uint8_t)((c >> 8) & 0xff), (uint8_t)(c & 0xff));
}

/* A vertical run at fixed x. Every fill below is built from it. */
static inline void ui_vspan(int x, int y, int h, uint32_t c) {
  if (!ui_bound || x < 0 || x >= UI_W) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > UI_H) h = UI_H - y;
  if (h <= 0) return;

  uint8_t b = (uint8_t)(c & 0xff), g = (uint8_t)((c >> 8) & 0xff), r = (uint8_t)((c >> 16) & 0xff);
  const ptrdiff_t ys = ui_s.y_stride;
  uint8_t *p = ui_s.base + (ptrdiff_t)x * ui_s.x_stride + (ptrdiff_t)y * ys;
  for (int i = 0; i < h; i++, p += ys) PLAT_PX(p, r, g, b);
}

static inline void ui_fill(int x, int y, int w, int h, uint32_t c) {
  if (x < 0) { w += x; x = 0; }
  if (x + w > UI_W) w = UI_W - x;
  for (int i = 0; i < w; i++) ui_vspan(x + i, y, h, c);
}

static inline void ui_clear(uint32_t c) { ui_fill(0, 0, UI_W, UI_H, c); }
static inline void ui_hline(int x, int y, int w, uint32_t c) { ui_fill(x, y, w, 1, c); }
static inline void ui_vline(int x, int y, int h, uint32_t c) { ui_vspan(x, y, h, c); }

/* How far in row `dy` of a corner of radius `r` is inset from the edge.
   Integer circle test on half-pixel centres, doubled to stay in ints. */
static inline int ui_inset(int r, int dy) {
  for (int dx = 0; dx <= r; dx++) {
    int a = 2 * (r - dx) - 1;
    int b = 2 * (r - dy) - 1;
    if (a * a + b * b <= 4 * r * r) return dx;
  }
  return r;
}

static inline void ui_rrect(int x, int y, int w, int h, int r, uint32_t c) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r <= 0) { ui_fill(x, y, w, h, c); return; }

  for (int dy = 0; dy < r; dy++) {
    int in = ui_inset(r, dy);
    ui_fill(x + in, y + dy, w - 2 * in, 1, c);
    ui_fill(x + in, y + h - 1 - dy, w - 2 * in, 1, c);
  }
  ui_fill(x, y + r, w, h - 2 * r, c);
}

/* One-pixel outline of a rounded rect, for borders and focus rings. */
static inline void ui_rrect_outline(int x, int y, int w, int h, int r, uint32_t c) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;

  for (int dy = 0; dy < r; dy++) {
    int in = ui_inset(r, dy);
    ui_px(x + in, y + dy, c);
    ui_px(x + w - 1 - in, y + dy, c);
    ui_px(x + in, y + h - 1 - dy, c);
    ui_px(x + w - 1 - in, y + h - 1 - dy, c);
    /* Fill the horizontal step between this row's inset and the last, or the
       corner arc comes out as disconnected dots on the shallow rows. */
    if (dy > 0) {
      int prev = ui_inset(r, dy - 1);
      for (int k = in + 1; k < prev; k++) {
        ui_px(x + k, y + dy, c);
        ui_px(x + w - 1 - k, y + dy, c);
        ui_px(x + k, y + h - 1 - dy, c);
        ui_px(x + w - 1 - k, y + h - 1 - dy, c);
      }
    }
  }
  ui_hline(x + r, y, w - 2 * r, c);
  ui_hline(x + r, y + h - 1, w - 2 * r, c);
  ui_vline(x, y + r, h - 2 * r, c);
  ui_vline(x + w - 1, y + r, h - 2 * r, c);
}

/* -------------------------------------------------------------------- text */

/* font8x8 covers ASCII 32-126 only; anything else draws as a space rather
   than indexing off the end of the table. */
static inline void ui_char(int x, int y, char ch, uint32_t c, int scale) {
  if (ch < 32 || ch > 126) ch = ' ';
  const unsigned char *g = font8x8[(int)ch - 32];
  for (int row = 0; row < 8; row++) {
    unsigned char bits = g[row];
    if (!bits) continue;
    for (int col = 0; col < 8; col++) {
      if (!(bits & (1 << col))) continue;
      if (scale == 1) ui_px(x + col, y + row, c);
      else ui_fill(x + col * scale, y + row * scale, scale, scale, c);
    }
  }
}

/* Transparent: only lit pixels are written, so text composites over whatever
   was painted underneath. */
static inline void ui_text(int x, int y, const char *s, uint32_t c, int scale) {
  for (; *s; s++, x += 8 * scale) {
    if (x >= UI_W) break;
    ui_char(x, y, *s, c, scale);
  }
}

static inline int ui_text_w(const char *s, int scale) {
  return (int)strlen(s) * 8 * scale;
}

static inline void ui_text_right(int right, int y, const char *s, uint32_t c, int scale) {
  ui_text(right - ui_text_w(s, scale), y, s, c, scale);
}

static inline void ui_text_centre(int x, int y, int w, const char *s, uint32_t c, int scale) {
  ui_text(x + (w - ui_text_w(s, scale)) / 2, y, s, c, scale);
}

/* Truncate with an ellipsis, so a long label cannot run under the value on
   its right. */
static inline void ui_text_clip(int x, int y, int maxw, const char *s, uint32_t c, int scale) {
  int cw = 8 * scale;
  int fits = maxw / cw;
  int len = (int)strlen(s);
  if (len <= fits) { ui_text(x, y, s, c, scale); return; }
  if (fits <= 3) return;
  for (int i = 0; i < fits - 3; i++) ui_char(x + i * cw, y, s[i], c, scale);
  for (int i = fits - 3; i < fits; i++) ui_char(x + i * cw, y, '.', c, scale);
}

/* ----------------------------------------------------------------- widgets */

/* Fill, hairline border, and a one-pixel top-edge highlight, per panoCard. No
   drop shadow: at this size and bit depth it turns into a smear. */
static inline void ui_card(const AdaPalette *p, int x, int y, int w, int h) {
  ui_rrect(x, y, w, h, UI_R_CARD, p->surface);
  ui_rrect_outline(x, y, w, h, UI_R_CARD, p->hl_med);
  ui_hline(x + UI_R_CARD, y + 1, w - 2 * UI_R_CARD, ada_lift(p, p->surface, 40));
}

/* The focus ring, matching the web one: 2px iris, offset 2px outside the
   thing it marks. */
static inline void ui_focus_ring(const AdaPalette *p, int x, int y, int w, int h, int r) {
  ui_rrect_outline(x - 2, y - 2, w + 4, h + 4, r + 2, p->iris);
  ui_rrect_outline(x - 3, y - 3, w + 6, h + 6, r + 3, p->iris);
}

/* Pill track with a travelling knob, per panoToggle. */
#define UI_TOGGLE_W 28
#define UI_TOGGLE_H 14

static inline void ui_toggle(const AdaPalette *p, int x, int y, bool on) {
  uint32_t track = on ? p->iris : p->hl_med;
  ui_rrect(x, y, UI_TOGGLE_W, UI_TOGGLE_H, UI_TOGGLE_H / 2, track);
  int kx = on ? x + UI_TOGGLE_W - UI_TOGGLE_H + 1 : x + 1;
  uint32_t knob = on ? (p->dark ? p->base : p->surface) : ada_lift(p, p->hl_high, 40);
  ui_rrect(kx, y + 1, UI_TOGGLE_H - 2, UI_TOGGLE_H - 2, (UI_TOGGLE_H - 2) / 2, knob);
}

typedef enum {
  UI_BTN_NORMAL,
  UI_BTN_SELECTED,   /* the current tab, or a chosen option */
  UI_BTN_DANGER,     /* destructive: the only place love is a fill */
  UI_BTN_DISABLED,
} UiButtonState;

static inline void ui_button(const AdaPalette *p, int x, int y, int w, int h,
                             const char *label, UiButtonState st) {
  uint32_t fill, ink;
  switch (st) {
    case UI_BTN_SELECTED: fill = p->iris;   ink = p->dark ? p->base : p->surface; break;
    case UI_BTN_DANGER:   fill = ada_sink(p, p->love, 120); ink = ada_lift(p, p->love, 110); break;
    case UI_BTN_DISABLED: fill = p->hl_low; ink = p->subtle; break;
    default:              fill = p->overlay; ink = p->text; break;
  }
  ui_rrect(x, y, w, h, UI_R_ROW, fill);
  if (st == UI_BTN_NORMAL || st == UI_BTN_DANGER)
    ui_rrect_outline(x, y, w, h, UI_R_ROW, p->hl_med);
  ui_text_centre(x, y + (h - 8) / 2, w, label, ink, 1);
}

/* The section tabs across the top, switched with L/R. */
static inline void ui_tabbar(const AdaPalette *p, int x, int y, int w, int h,
                             const char *const *labels, int count, int active) {
  ui_fill(x, y, w, h, p->base);
  int tw = w / count;
  for (int i = 0; i < count; i++) {
    int tx = x + i * tw;
    bool on = (i == active);
    ui_text_centre(tx, y + (h - 8) / 2 - 1, tw, labels[i],
                   on ? p->text : p->muted, 1);
    /* An underline, not a filled pill: at 320px wide four filled tabs leave
       no room for the labels. */
    if (on) ui_fill(tx + 6, y + h - 3, tw - 12, 2, p->iris);
  }
  ui_hline(x, y + h - 1, w, p->hl_med);
}

/* A thin scrollbar down the right edge of a list, drawn only when the content
   overflows. */
static inline void ui_scrollbar(const AdaPalette *p, int x, int y, int h,
                                int offset, int content, int view) {
  if (content <= view) return;
  ui_rrect(x, y, 3, h, 1, p->hl_low);
  int th = h * view / content;
  if (th < 8) th = 8;
  int ty = (content == view) ? 0 : (h - th) * offset / (content - view);
  ui_rrect(x, y + ty, 3, th, 1, p->hl_high);
}

/* A list row. `value` may be NULL for an action row. */
static inline void ui_row(const AdaPalette *p, int x, int y, int w, int h,
                          const char *label, const char *value,
                          bool focused, bool disabled) {
  if (focused) ui_rrect(x, y, w, h, UI_R_ROW, p->overlay);

  uint32_t ink = disabled ? p->subtle : p->text;
  uint32_t val = disabled ? p->subtle : (focused ? p->iris : p->muted);

  int vw = value ? ui_text_w(value, 1) : 0;
  ui_text_clip(x + 8, y + (h - 8) / 2, w - 24 - vw, label, ink, 1);
  if (value) ui_text_right(x + w - 8, y + (h - 8) / 2, value, val, 1);
}

#endif /* CORE_UI_H */
