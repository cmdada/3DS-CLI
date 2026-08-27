#ifndef PLAT_KBD_H
#define PLAT_KBD_H

/* The panel keyboard, behind source/core/plat.h's pkbd_* seam.
 *
 * Nothing on this console can point at the panel, so the grid is walked with
 * the d-pad and committed with cross, i already did this on gamecube so
 * nothing revolutionary here
 */

#include <ctrosk.h>

#include "plat.h"
#include "theme.h"
#include "osk_theme.h"

static CtrOsk      g_osk;
static CtrOskTheme g_osk_theme;

void pkbd_init(void) {
  ctrOskInit(&g_osk);
  g_osk.title    = "3ds-cli";
  g_osk.subtitle = "adabit.org";
  ada_osk_fit(&g_osk, PLAT_PANEL_W, PLAT_PANEL_H);
}

void pkbd_apply(const AdaPalette *p, bool backspace_del, bool shift_oneshot, int mode) {
  (void)mode;
  ada_osk_theme(p, &g_osk_theme);
  g_osk.theme            = &g_osk_theme;
  g_osk.backspace_as_del = backspace_del;
  g_osk.shift_is_oneshot = shift_oneshot;
  ctrOskInvalidate(&g_osk);
}

void pkbd_invalidate(void) { ctrOskInvalidate(&g_osk); }

void pkbd_update(const plat_input_t *in) {
  CtrOskEvent ev;
  int dr = 0, dc = 0;
  if (in->down & PLAT_BTN_UP)    dr = -1;
  if (in->down & PLAT_BTN_DOWN)  dr = +1;
  if (in->down & PLAT_BTN_LEFT)  dc = -1;
  if (in->down & PLAT_BTN_RIGHT) dc = +1;
  if (dr || dc) ctrOskMoveFocus(&g_osk, dr, dc);
  if ((in->down & PLAT_BTN_A) && ctrOskActivate(&g_osk, &ev))
    rx_push((char)ev.byte);
}

void pkbd_draw(void) {
  plat_fb_t fb;
  if (!plat_surface(PLAT_SURF_PANEL, &fb)) return;
  /* VRAM here is 8888 with red in the low byte; see PLAT_PIXEL_RGB. */
  CtrOskSurface dst = { fb.base, fb.w, fb.h, fb.x_stride, fb.y_stride, 1 };
  ctrOskDraw(&g_osk, &dst);
}

#endif /* PLAT_KBD_H */
