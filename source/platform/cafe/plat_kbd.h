#ifndef PLAT_KBD_H
#define PLAT_KBD_H

/* The GamePad keyboard, behind source/core/plat.h's pkbd_* seam.
 *
 * The GamePad is a touch panel, so this is the same realtime keyboard the 3DS
 * uses (vendor/ctr-osk-rt) drawing into the same logical 320x240 surface. The
 * Wii U has no always-available line-compose applet, so there is only the one
 * mode here; caps.swkbd is false and core never asks for the other.
 */

#include <ctrosk.h>

#include "plat.h"
#include "theme.h"

static CtrOsk      g_osk;
static CtrOskTheme g_osk_theme;

#include "osk_theme.h"

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
  CtrOskPointer p = { in->ptr_x, in->ptr_y, in->ptr_down };
  CtrOskEvent ev;
  if (ctrOskUpdate(&g_osk, &p, &ev)) rx_push((char)ev.byte);
}

void pkbd_draw(void) {
  plat_fb_t fb;
  if (!plat_surface(PLAT_SURF_PANEL, &fb)) return;
  /* OSScreen stores red first; see PLAT_PIXEL_RGB in plat_cfg.h. */
  CtrOskSurface dst = { fb.base, fb.w, fb.h, fb.x_stride, fb.y_stride, 1 };
  ctrOskDraw(&g_osk, &dst);
}

#endif /* PLAT_KBD_H */
