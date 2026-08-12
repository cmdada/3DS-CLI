#ifndef CONFIG_H
#define CONFIG_H

/* Persistent settings, at sdmc:/3ds-cli.cfg.

   Plain `key=value` text, so a setting that makes the app unusable can be
   fixed by hand rather than only through the broken UI. Every field is
   declared once, in cfg_fields[] below; load, save and the settings page all
   drive off that table. */

#include <3ds.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "theme.h"

#define CFG_PATH "sdmc:/3ds-cli.cfg"

typedef struct {
  /* Display. Applied live; safe to change while the guest runs. */
  int  theme;          /* ADA_THEME_*                                       */
  bool top_black;      /* terminal background: pure black, not theme base   */
  bool use_5x7;        /* false = 8x8 font, true = 5x7                      */
  int  zoom_x, zoom_y; /* 1..5, independent (terminal.h applies them so)    */
  bool cursor_blink;
  bool follow_output;  /* keep the viewport pinned to the cursor            */

  /* Input. Applied live. */
  int  keyboard;       /* 0 = realtime (ctr-osk-rt), 1 = 3DS system keyboard */
  bool backspace_del;  /* backspace sends DEL 127 rather than BS 8           */
  bool shift_oneshot;
  bool circle_pans;    /* circle pad pans the viewport, else sends arrows    */

  /* Hardware. Read once at boot into a snapshot; see main.c. Changing one
     mid-session does nothing until the next launch: a virtio device vanishing
     under a driver that has bound to it is a guest hang. */
  bool dev_net, dev_sd, dev_nand, dev_twl, dev_sensors, dev_rng, dev_swap;
  int  ram_cap_mb;     /* 0 = auto (walk malloc down from 64MB)             */

  /* Set by the settings page, consumed and cleared by the next boot: the page
     cannot unlink rootfs.ext2 while the emulator thread has it open. */
  bool reset_rootfs;
} Cfg;

typedef enum { CFG_BOOL, CFG_INT } CfgType;

typedef struct {
  const char *key;
  CfgType     type;
  size_t      off;
  int         lo, hi;  /* inclusive; ints out of range fall back to default */
} CfgField;

#define CFG_F(name, type, lo, hi) { #name, type, offsetof(Cfg, name), lo, hi }

static const CfgField cfg_fields[] = {
  CFG_F(theme,         CFG_INT,  0, ADA_THEME_COUNT - 1),
  CFG_F(top_black,     CFG_BOOL, 0, 1),
  CFG_F(use_5x7,       CFG_BOOL, 0, 1),
  CFG_F(zoom_x,        CFG_INT,  1, 5),
  CFG_F(zoom_y,        CFG_INT,  1, 5),
  CFG_F(cursor_blink,  CFG_BOOL, 0, 1),
  CFG_F(follow_output, CFG_BOOL, 0, 1),

  CFG_F(keyboard,      CFG_INT,  0, 1),
  CFG_F(backspace_del, CFG_BOOL, 0, 1),
  CFG_F(shift_oneshot, CFG_BOOL, 0, 1),
  CFG_F(circle_pans,   CFG_BOOL, 0, 1),

  CFG_F(dev_net,       CFG_BOOL, 0, 1),
  CFG_F(dev_sd,        CFG_BOOL, 0, 1),
  CFG_F(dev_nand,      CFG_BOOL, 0, 1),
  CFG_F(dev_twl,       CFG_BOOL, 0, 1),
  CFG_F(dev_sensors,   CFG_BOOL, 0, 1),
  CFG_F(dev_rng,       CFG_BOOL, 0, 1),
  CFG_F(dev_swap,      CFG_BOOL, 0, 1),
  CFG_F(ram_cap_mb,    CFG_INT,  0, 64),

  CFG_F(reset_rootfs,  CFG_BOOL, 0, 1),
};

#define CFG_NFIELDS ((int)(sizeof(cfg_fields) / sizeof(cfg_fields[0])))

static inline bool *cfg_boolp(Cfg *c, const CfgField *f) {
  return (bool *)((char *)c + f->off);
}
static inline int *cfg_intp(Cfg *c, const CfgField *f) {
  return (int *)((char *)c + f->off);
}

/* Defaults reproduce the app's pre-settings behaviour, except that `theme`
   starts on the house palette rather than the old hardcoded colours. */
static inline void cfg_defaults(Cfg *c) {
  memset(c, 0, sizeof(*c));
  c->theme         = ADA_THEME_DARK;
  c->top_black     = false;
  c->use_5x7       = false;   /* term_init's default                        */
  c->zoom_x        = 1;
  c->zoom_y        = 1;
  c->cursor_blink  = true;
  c->follow_output = true;    /* WriteUARTByte used to force this on always */

  c->keyboard      = 0;       /* ctr-osk-rt                                 */
  c->backspace_del = true;    /* ctrOskInit's default: terminals want DEL   */
  c->shift_oneshot = true;    /* ctrOskInit's default                       */
  c->circle_pans   = true;

  c->dev_net = c->dev_sd = c->dev_nand = c->dev_twl = true;
  c->dev_sensors = c->dev_rng = c->dev_swap = true;
  c->ram_cap_mb    = 0;       /* auto                                       */

  c->reset_rootfs  = false;
}

static inline char *cfg_trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
  *e = '\0';
  return s;
}

/* Defaults go in first, so a missing, truncated or older file still gives a
   complete Cfg. An unknown, unparseable or out-of-range key is skipped and
   keeps its default; nothing here can fail the app's startup. */
static inline void cfg_load(Cfg *c) {
  cfg_defaults(c);

  FILE *f = fopen(CFG_PATH, "r");
  if (!f) return;

  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char *p = cfg_trim(line);
    if (*p == '\0' || *p == '#') continue;

    char *eq = strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = cfg_trim(p);
    char *val = cfg_trim(eq + 1);

    for (int i = 0; i < CFG_NFIELDS; i++) {
      const CfgField *fd = &cfg_fields[i];
      if (strcmp(key, fd->key) != 0) continue;

      char *end = NULL;
      long v = strtol(val, &end, 10);
      if (end == val || *end != '\0') break;      /* not a number: keep default */
      if (v < fd->lo || v > fd->hi) break;        /* out of range: keep default */

      if (fd->type == CFG_BOOL) *cfg_boolp(c, fd) = (v != 0);
      else                      *cfg_intp(c, fd)  = (int)v;
      break;
    }
  }
  fclose(f);
}

/* Called when the settings page closes and on the exit path, never per
   keypress: an SD write stalls the emulation thread for as long as the card
   takes. */
static inline bool cfg_save(const Cfg *c) {
  FILE *f = fopen(CFG_PATH, "w");
  if (!f) return false;

  fprintf(f, "# 3ds-cli settings. Edit by hand if the in-app page is unusable;\n"
             "# delete this file to go back to defaults.\n");
  for (int i = 0; i < CFG_NFIELDS; i++) {
    const CfgField *fd = &cfg_fields[i];
    Cfg *m = (Cfg *)c;  /* the accessors are shared with the mutable path */
    int v = (fd->type == CFG_BOOL) ? (*cfg_boolp(m, fd) ? 1 : 0) : *cfg_intp(m, fd);
    fprintf(f, "%s=%d\n", fd->key, v);
  }
  fclose(f);
  return true;
}

#endif /* CONFIG_H */
