#ifndef PLAT_KBD_H
#define PLAT_KBD_H

/* The 3DS's two panel keyboards, behind source/core/plat.h's pkbd_* seam.
 *
 * Mode 0 is vendor/ctr-osk-rt: a realtime touch grid that sits on the bottom
 * screen permanently and hands back one key at a time. Mode 1 is Horizon's own
 * swkbd applet, which takes over both screens and blocks until the user is
 * done, so it lives behind a prompt button rather than being always-on - see
 * inputpane.h.
 */

#include <ctrosk.h>

#include "plat.h"
#include "theme.h"
#include "inputpane.h"

static CtrOsk      g_osk;
static CtrOskTheme g_osk_theme;
static int         g_kbd_mode;

#include "osk_theme.h"

void pkbd_init(void) {
  /* ctrOskInit is also what turns off double buffering down there, since it
     redraws in place and never swaps. */
  ctrOskInit(&g_osk);
  g_osk.title    = "3ds-cli";
  g_osk.subtitle = "adabit.org";
  ada_osk_fit(&g_osk, PLAT_PANEL_W, PLAT_PANEL_H);
}

void pkbd_apply(const AdaPalette *p, bool backspace_del, bool shift_oneshot, int mode) {
  g_kbd_mode = mode;
  ada_osk_theme(p, &g_osk_theme);
  g_osk.theme            = &g_osk_theme;
  g_osk.backspace_as_del = backspace_del;
  g_osk.shift_is_oneshot = shift_oneshot;
  ctrOskInvalidate(&g_osk);
}

void pkbd_invalidate(void) { ctrOskInvalidate(&g_osk); }

void pkbd_update(const plat_input_t *in) {
  if (g_kbd_mode == 0) {
    /* The keyboard does its own edge detection off the pointer state the input
       poll just sampled, so the only thing left is to forward the byte. */
    CtrOskPointer p = { in->ptr_x, in->ptr_y, in->ptr_down };
    CtrOskEvent ev;
    if (ctrOskUpdate(&g_osk, &p, &ev)) rx_push((char)ev.byte);
    return;
  }
  /* pane_prompt suspends this process for as long as the applet is up - see
     the clock re-basing it arranges on the way out. */
  if (in->down & PLAT_BTN_SWKBD) pane_prompt();
  else if (in->ptr_tapped)       pane_touch(in);
}

/* Both modes are dirty-tracked and only repaint when something changed: a
   full bottom-screen repaint is not free on an Old 3DS. */
void pkbd_draw(void) {
  if (g_kbd_mode != 0) { pane_draw(); return; }

  plat_fb_t fb;
  if (!plat_surface(PLAT_SURF_PANEL, &fb)) return;
  CtrOskSurface dst = { fb.base, fb.w, fb.h, fb.x_stride, fb.y_stride, 0 };
  ctrOskDraw(&g_osk, &dst);
}

#endif /* PLAT_KBD_H */
