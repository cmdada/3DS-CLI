#ifndef OSK_THEME_H
#define OSK_THEME_H

/* Palette -> ctrosk theme. Shared by every console's plat_kbd.h: the keyboard
   and the palette are both platform-neutral, only the surface differs.

   The key/mod highlight and shadow pairs are computed with ada_lift/ada_sink
   rather than left to ctrosk's additive shade, which does nothing visible on a
   light face. */

static inline CtrOskColor ada_osk(uint32_t c) {
  CtrOskColor o = { (uint8_t)((c >> 16) & 0xff), (uint8_t)((c >> 8) & 0xff), (uint8_t)(c & 0xff) };
  return o;
}

static void ada_osk_theme(const AdaPalette *p, CtrOskTheme *t) {
  uint32_t key = p->overlay;
  uint32_t mod = ada_mix(p->overlay, p->surface, 128);

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

/* Fit the key grid to the panel. The library's default geometry is sized for
   a 320x240 screen; a console with a wider or taller panel wants bigger keys,
   not the same small grid in a corner. `y` is the status bar's height and
   stays fixed - it holds one line of text whatever the panel is. */
static inline void ada_osk_fit(CtrOsk *osk, int w, int h) {
  const int gap = 2, x = 1, top = 38;
  osk->geom.gap   = gap;
  osk->geom.x     = x;
  osk->geom.y     = top;
  osk->geom.key_w = (w - 2 * x - (CTR_OSK_COLS - 1) * gap) / CTR_OSK_COLS;
  osk->geom.key_h = (h - top - (CTR_OSK_ROWS - 1) * gap) / CTR_OSK_ROWS;
}

#endif /* OSK_THEME_H */
