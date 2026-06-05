#ifndef TERMINAL_H
#define TERMINAL_H

#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "glcdfont.h"
#include "draw.h" // for font8x8

#define TERM_COLS 80
#define TERM_ROWS 30
#define TERM_SCROLLBACK 200

#define TERM_FLAG_BOLD      (1 << 0)
#define TERM_FLAG_DIM       (1 << 1)
#define TERM_FLAG_REVERSE   (1 << 2)
#define TERM_FLAG_UNDERLINE (1 << 3)

enum TermParserState {
  STATE_NORMAL,
  STATE_ESC,
  STATE_CSI
};

typedef struct {
  u32 fg;
  u32 bg;
  char c;
  u8 flags;
} TermCell;

typedef struct {
  TermCell grid[TERM_COLS * TERM_ROWS];

  TermCell sb_buf[TERM_SCROLLBACK][TERM_COLS];
  int sb_head; 
  int sb_count;  

  int cx, cy;
  int saved_cx, saved_cy;
  u32 cur_fg;
  u32 cur_bg;
  u8 cur_flags;

  u32 default_fg;
  u32 default_bg;

  int scroll_top;
  int scroll_bottom;

  // Escape parser state
  int state;
  int params[16];
  int param_cnt;
  bool has_param;
  bool private_mode;

  // Viewport & Zoom
  int zoom_x;
  int zoom_y;
  int scroll_x;
  int scroll_y;
  bool auto_track;
  bool use_5x7; // true = 5x7 font, false = 8x8 font
  bool cursor_visible;
  bool dirty;
} TermState;

// 16 Standard ANSI Colors resolved to RGB
static const u32 ansi_colors[16] = {
  // Normal colors
  0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
  0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
  // Bright colors
  0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
  0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff
};

static inline u32 xterm_256_to_rgb(u8 index) {
  if (index < 8) {
    return ansi_colors[index];
  } else if (index < 16) {
    return ansi_colors[index];
  } else if (index < 232) {
    // 6x6x6 color cube
    int r = ((index - 16) / 36) * 51;
    int g = (((index - 16) / 6) % 6) * 51;
    int b = ((index - 16) % 6) * 51;
    return (r << 16) | (g << 8) | b;
  } else {
    // Grayscale ramp
    int val = 8 + (index - 232) * 10;
    return (val << 16) | (val << 8) | val;
  }
}

static inline void term_scroll_up(TermState *ts, int top, int bottom) {
  if (top < 0 || bottom >= TERM_ROWS || top >= bottom) return;

  if (top == 0) {
    int slot = (ts->sb_head + ts->sb_count) % TERM_SCROLLBACK;
    memcpy(ts->sb_buf[slot], &ts->grid[top * TERM_COLS], TERM_COLS * sizeof(TermCell));
    if (ts->sb_count < TERM_SCROLLBACK) {
      ts->sb_count++;
    } else {
      ts->sb_head = (ts->sb_head + 1) % TERM_SCROLLBACK;
    }
  }

  for (int y = top; y < bottom; y++) {
    memcpy(&ts->grid[y * TERM_COLS], &ts->grid[(y + 1) * TERM_COLS], TERM_COLS * sizeof(TermCell));
  }
  TermCell clear_cell = { ts->default_fg, ts->default_bg, ' ', 0 };
  for (int x = 0; x < TERM_COLS; x++) {
    ts->grid[bottom * TERM_COLS + x] = clear_cell;
  }
}

static inline void term_init(TermState *ts) {
  ts->default_fg = 0xe6e6eb;
  ts->default_bg = 0x1e1e23;
  ts->cur_fg = ts->default_fg;
  ts->cur_bg = ts->default_bg;
  ts->cur_flags = 0;

  ts->cx = 0;
  ts->cy = 0;
  ts->saved_cx = 0;
  ts->saved_cy = 0;

  ts->scroll_top = 0;
  ts->scroll_bottom = TERM_ROWS - 1;

  ts->sb_head  = 0;
  ts->sb_count = 0;
  memset(ts->sb_buf, 0, sizeof(ts->sb_buf));

  ts->state = STATE_NORMAL;
  ts->zoom_x = 1;
  ts->zoom_y = 1;
  ts->scroll_x = 0;
  ts->scroll_y = 0;
  ts->auto_track = true;
  ts->use_5x7 = false;
  ts->cursor_visible = true;
  ts->dirty = true;

  for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
    ts->grid[i].c = ' ';
    ts->grid[i].fg = ts->default_fg;
    ts->grid[i].bg = ts->default_bg;
    ts->grid[i].flags = 0;
  }
}

static inline void term_handle_sgr(TermState *ts) {
  if (ts->param_cnt == 0) {
    ts->cur_fg = ts->default_fg;
    ts->cur_bg = ts->default_bg;
    ts->cur_flags = 0;
    return;
  }

  for (int i = 0; i < ts->param_cnt; i++) {
    int p = ts->params[i];
    if (p == 0) {
      ts->cur_fg = ts->default_fg;
      ts->cur_bg = ts->default_bg;
      ts->cur_flags = 0;
    } else if (p == 1) {
      ts->cur_flags |= TERM_FLAG_BOLD;
    } else if (p == 2) {
      ts->cur_flags |= TERM_FLAG_DIM;
    } else if (p == 4) {
      ts->cur_flags |= TERM_FLAG_UNDERLINE;
    } else if (p == 7) {
      ts->cur_flags |= TERM_FLAG_REVERSE;
    } else if (p == 22) {
      ts->cur_flags &= ~(TERM_FLAG_BOLD | TERM_FLAG_DIM);
    } else if (p == 24) {
      ts->cur_flags &= ~TERM_FLAG_UNDERLINE;
    } else if (p == 27) {
      ts->cur_flags &= ~TERM_FLAG_REVERSE;
    } else if (p >= 30 && p <= 37) {
      ts->cur_fg = ansi_colors[p - 30];
    } else if (p == 38) {
      // Foreground color options
      if (i + 2 < ts->param_cnt && ts->params[i+1] == 5) {
        ts->cur_fg = xterm_256_to_rgb(ts->params[i+2]);
        i += 2;
      } else if (i + 4 < ts->param_cnt && ts->params[i+1] == 2) {
        ts->cur_fg = (ts->params[i+2] << 16) | (ts->params[i+3] << 8) | ts->params[i+4];
        i += 4;
      }
    } else if (p == 39) {
      ts->cur_fg = ts->default_fg;
    } else if (p >= 40 && p <= 47) {
      ts->cur_bg = ansi_colors[p - 40];
    } else if (p == 48) {
      // Background color options
      if (i + 2 < ts->param_cnt && ts->params[i+1] == 5) {
        ts->cur_bg = xterm_256_to_rgb(ts->params[i+2]);
        i += 2;
      } else if (i + 4 < ts->param_cnt && ts->params[i+1] == 2) {
        ts->cur_bg = (ts->params[i+2] << 16) | (ts->params[i+3] << 8) | ts->params[i+4];
        i += 4;
      }
    } else if (p == 49) {
      ts->cur_bg = ts->default_bg;
    } else if (p >= 90 && p <= 97) {
      ts->cur_fg = ansi_colors[p - 90 + 8];
    } else if (p >= 100 && p <= 107) {
      ts->cur_bg = ansi_colors[p - 100 + 8];
    }
  }
}

static inline void term_handle_csi(TermState *ts, char cmd) {
  int n = (ts->param_cnt > 0 && ts->params[0] > 0) ? ts->params[0] : 1;

  switch (cmd) {
    case 'H':
    case 'f': {
      int row = (ts->param_cnt > 0 && ts->params[0] > 0) ? ts->params[0] : 1;
      int col = (ts->param_cnt > 1 && ts->params[1] > 0) ? ts->params[1] : 1;
      ts->cy = row - 1;
      ts->cx = col - 1;
      if (ts->cx >= TERM_COLS) ts->cx = TERM_COLS - 1;
      if (ts->cy >= TERM_ROWS) ts->cy = TERM_ROWS - 1;
      if (ts->cx < 0) ts->cx = 0;
      if (ts->cy < 0) ts->cy = 0;
      break;
    }
    case 'A': // Up
      ts->cy -= n;
      if (ts->cy < 0) ts->cy = 0;
      break;
    case 'B': // Down
      ts->cy += n;
      if (ts->cy >= TERM_ROWS) ts->cy = TERM_ROWS - 1;
      break;
    case 'C': // Right
      ts->cx += n;
      if (ts->cx >= TERM_COLS) ts->cx = TERM_COLS - 1;
      break;
    case 'D': // Left
      ts->cx -= n;
      if (ts->cx < 0) ts->cx = 0;
      break;
    case 'E': // Next Line
      ts->cx = 0;
      ts->cy += n;
      if (ts->cy >= TERM_ROWS) ts->cy = TERM_ROWS - 1;
      break;
    case 'F': // Prev Line
      ts->cx = 0;
      ts->cy -= n;
      if (ts->cy < 0) ts->cy = 0;
      break;
    case 'G': // Cursor Horizontal Absolute
      ts->cx = n - 1;
      if (ts->cx < 0) ts->cx = 0;
      if (ts->cx >= TERM_COLS) ts->cx = TERM_COLS - 1;
      break;
    case 'J': { // Erase in Display
      int mode = (ts->param_cnt > 0) ? ts->params[0] : 0;
      if (mode == 0) {
        int start = ts->cy * TERM_COLS + ts->cx;
        for (int i = start; i < TERM_COLS * TERM_ROWS; i++) {
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->default_fg;
          ts->grid[i].bg = ts->default_bg;
          ts->grid[i].flags = 0;
        }
      } else if (mode == 1) {
        int end = ts->cy * TERM_COLS + ts->cx;
        for (int i = 0; i <= end; i++) {
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->default_fg;
          ts->grid[i].bg = ts->default_bg;
          ts->grid[i].flags = 0;
        }
      } else if (mode == 2 || mode == 3) {
        for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->default_fg;
          ts->grid[i].bg = ts->default_bg;
          ts->grid[i].flags = 0;
        }
      }
      break;
    }
    case 'K': { // Erase in Line
      int mode = (ts->param_cnt > 0) ? ts->params[0] : 0;
      if (mode == 0) {
        for (int x = ts->cx; x < TERM_COLS; x++) {
          int i = ts->cy * TERM_COLS + x;
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->default_fg;
          ts->grid[i].bg = ts->default_bg;
          ts->grid[i].flags = 0;
        }
      } else if (mode == 1) {
        for (int x = 0; x <= ts->cx; x++) {
          int i = ts->cy * TERM_COLS + x;
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->default_fg;
          ts->grid[i].bg = ts->default_bg;
          ts->grid[i].flags = 0;
        }
      } else if (mode == 2) {
        for (int x = 0; x < TERM_COLS; x++) {
          int i = ts->cy * TERM_COLS + x;
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->default_fg;
          ts->grid[i].bg = ts->default_bg;
          ts->grid[i].flags = 0;
        }
      }
      break;
    }
    case 'S': // Scroll up
      for (int i = 0; i < n; i++) {
        term_scroll_up(ts, ts->scroll_top, ts->scroll_bottom);
      }
      break;
    case 'r': { // Set Scrolling Region
      int top = (ts->param_cnt > 0 && ts->params[0] > 0) ? ts->params[0] : 1;
      int bottom = (ts->param_cnt > 1 && ts->params[1] > 0) ? ts->params[1] : TERM_ROWS;
      ts->scroll_top = top - 1;
      ts->scroll_bottom = bottom - 1;
      if (ts->scroll_top < 0) ts->scroll_top = 0;
      if (ts->scroll_bottom >= TERM_ROWS) ts->scroll_bottom = TERM_ROWS - 1;
      ts->cx = 0;
      ts->cy = 0;
      break;
    }
    case 'm': // SGR
      term_handle_sgr(ts);
      break;
    case 's': // Save cursor
      ts->saved_cx = ts->cx;
      ts->saved_cy = ts->cy;
      break;
    case 'u': // Restore cursor
      ts->cx = ts->saved_cx;
      ts->cy = ts->saved_cy;
      break;
    case 'h': // Set Mode
    case 'l': // Reset Mode
      if (ts->private_mode && ts->param_cnt > 0 && ts->params[0] == 25) {
        ts->cursor_visible = (cmd == 'h');
      }
      break;
  }
}

static inline void term_write_char(TermState *ts, char c) {
  ts->dirty = true;

  switch (ts->state) {
    case STATE_NORMAL:
      if (c == 27) {
        ts->state = STATE_ESC;
      } else if (c == '\n') {
        ts->cx = 0;
        ts->cy++;
        if (ts->cy > ts->scroll_bottom) {
          term_scroll_up(ts, ts->scroll_top, ts->scroll_bottom);
          ts->cy = ts->scroll_bottom;
        }
      } else if (c == '\r') {
        ts->cx = 0;
      } else if (c == '\b') {
        if (ts->cx > 0) ts->cx--;
      } else if (c == '\t') {
        int next_tab = (ts->cx + 8) & ~7;
        while (ts->cx < next_tab && ts->cx < TERM_COLS) {
          int i = ts->cy * TERM_COLS + ts->cx;
          ts->grid[i].c = ' ';
          ts->grid[i].fg = ts->cur_fg;
          ts->grid[i].bg = ts->cur_bg;
          ts->grid[i].flags = ts->cur_flags;
          ts->cx++;
        }
        if (ts->cx >= TERM_COLS) {
          ts->cx = 0;
          ts->cy++;
          if (ts->cy > ts->scroll_bottom) {
            term_scroll_up(ts, ts->scroll_top, ts->scroll_bottom);
            ts->cy = ts->scroll_bottom;
          }
        }
      } else if (c >= 32 && c <= 126) {
        if (ts->cx >= TERM_COLS) {
          ts->cx = 0;
          ts->cy++;
          if (ts->cy > ts->scroll_bottom) {
            term_scroll_up(ts, ts->scroll_top, ts->scroll_bottom);
            ts->cy = ts->scroll_bottom;
          }
        }
        int i = ts->cy * TERM_COLS + ts->cx;
        ts->grid[i].c = c;
        ts->grid[i].fg = ts->cur_fg;
        ts->grid[i].bg = ts->cur_bg;
        ts->grid[i].flags = ts->cur_flags;
        ts->cx++;
      }
      break;

    case STATE_ESC:
      if (c == '[') {
        ts->state = STATE_CSI;
        ts->param_cnt = 0;
        ts->params[0] = 0;
        ts->has_param = false;
        ts->private_mode = false;
      } else {
        ts->state = STATE_NORMAL;
      }
      break;

    case STATE_CSI:
      if (c >= '0' && c <= '9') {
        ts->params[ts->param_cnt] = ts->params[ts->param_cnt] * 10 + (c - '0');
        ts->has_param = true;
      } else if (c == ';') {
        if (ts->param_cnt < 15) {
          ts->param_cnt++;
          ts->params[ts->param_cnt] = 0;
        }
      } else if (c == '?') {
        ts->private_mode = true;
      } else if (c >= 0x40 && c <= 0x7E) {
        if (ts->has_param) {
          ts->param_cnt++;
        }
        term_handle_csi(ts, c);
        ts->state = STATE_NORMAL;
      } else {
        ts->state = STATE_NORMAL;
      }
      break;
  }
}

static inline void term_draw_char_5x7(u8 *fb, int px, int py, char c, u32 fg, u32 bg, int zx, int zy, u8 flags) {
  if (c < 32 || c > 127) c = ' ';
  int glyph_idx = (unsigned char)c;
  const unsigned char *glyph = &font5x7[glyph_idx * 5];

  bool is_reverse = (flags & TERM_FLAG_REVERSE);
  u32 draw_fg = is_reverse ? bg : fg;
  u32 draw_bg = is_reverse ? fg : bg;

  u8 fr = (draw_fg >> 16) & 0xff;
  u8 fg_g = (draw_fg >> 8) & 0xff;
  u8 fb_c = draw_fg & 0xff;

  u8 br = (draw_bg >> 16) & 0xff;
  u8 bg_g = (draw_bg >> 8) & 0xff;
  u8 bb_c = draw_bg & 0xff;

  for (int col = 0; col < 6; col++) {
    unsigned char bits = (col < 5) ? glyph[col] : 0;
    for (int row = 0; row < 8; row++) {
      bool pixel_on = (bits & (1 << row)) != 0;
      u8 r = pixel_on ? fr : br;
      u8 g = pixel_on ? fg_g : bg_g;
      u8 b = pixel_on ? fb_c : bb_c;

      if (flags & TERM_FLAG_DIM && pixel_on) {
        r /= 2; g /= 2; b /= 2;
      }
      if (flags & TERM_FLAG_UNDERLINE && row == 7) {
        r = fr; g = fg_g; b = fb_c;
      }

      for (int dy = 0; dy < zy; dy++) {
        int ry = py + row * zy + dy;
        if (ry < 0 || ry >= 240) continue;
        for (int dx = 0; dx < zx; dx++) {
          int rx = px + col * zx + dx;
          if (rx < 0 || rx >= 400) continue;
          u32 off = (rx * 240 + (239 - ry)) * 3;
          fb[off] = b; fb[off+1] = g; fb[off+2] = r;
        }
      }
    }
  }
}

static inline void term_draw_char_8x8(u8 *fb, int px, int py, char c, u32 fg, u32 bg, int zx, int zy, u8 flags) {
  if (c < 32 || c > 126) c = ' ';
  const unsigned char *glyph = font8x8[(int)c - 32];

  bool is_reverse = (flags & TERM_FLAG_REVERSE);
  u32 draw_fg = is_reverse ? bg : fg;
  u32 draw_bg = is_reverse ? fg : bg;

  u8 fr = (draw_fg >> 16) & 0xff;
  u8 fg_g = (draw_fg >> 8) & 0xff;
  u8 fb_c = draw_fg & 0xff;

  u8 br = (draw_bg >> 16) & 0xff;
  u8 bg_g = (draw_bg >> 8) & 0xff;
  u8 bb_c = draw_bg & 0xff;

  for (int row = 0; row < 8; row++) {
    unsigned char bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      bool pixel_on = (bits & (1 << col)) != 0;
      u8 r = pixel_on ? fr : br;
      u8 g = pixel_on ? fg_g : bg_g;
      u8 b = pixel_on ? fb_c : bb_c;

      if (flags & TERM_FLAG_DIM && pixel_on) {
        r /= 2; g /= 2; b /= 2;
      }
      if (flags & TERM_FLAG_UNDERLINE && row == 7) {
        r = fr; g = fg_g; b = fb_c;
      }

      for (int dy = 0; dy < zy; dy++) {
        int ry = py + row * zy + dy;
        if (ry < 0 || ry >= 240) continue;
        for (int dx = 0; dx < zx; dx++) {
          int rx = px + col * zx + dx;
          if (rx < 0 || rx >= 400) continue;
          u32 off = (rx * 240 + (239 - ry)) * 3;
          fb[off] = b; fb[off+1] = g; fb[off+2] = r;
        }
      }
    }
  }
}

static inline void term_draw(TermState *ts, u8 *fb) {
  int font_w = ts->use_5x7 ? 5 : 8;
  int font_h = 8;
  int char_w = font_w * ts->zoom_x;
  int char_h = font_h * ts->zoom_y;

  int vis_cols = 400 / char_w;
  if (vis_cols > TERM_COLS) vis_cols = TERM_COLS;
  int vis_rows = 240 / char_h;
  if (vis_rows > TERM_ROWS) vis_rows = TERM_ROWS;

  if (ts->auto_track) {
    if (ts->cx < ts->scroll_x) {
      ts->scroll_x = ts->cx;
    } else if (ts->cx >= ts->scroll_x + vis_cols) {
      ts->scroll_x = ts->cx - vis_cols + 1;
    }
    if (ts->cy < ts->scroll_y) {
      ts->scroll_y = ts->cy;
    } else if (ts->cy >= ts->scroll_y + vis_rows) {
      ts->scroll_y = ts->cy - vis_rows + 1;
    }
  }

  // Clamp scroll_x to live grid bounds
  int max_scroll_x = TERM_COLS - vis_cols;
  if (max_scroll_x < 0) max_scroll_x = 0;
  if (ts->scroll_x > max_scroll_x) ts->scroll_x = max_scroll_x;
  if (ts->scroll_x < 0) ts->scroll_x = 0;

  // scroll_y: negative means we are looking at scrollback history.
  // The furthest back we can go is -(sb_count) rows (oldest saved line).
  int min_scroll_y = -ts->sb_count;
  int max_scroll_y = TERM_ROWS - vis_rows;
  if (max_scroll_y < 0) max_scroll_y = 0;
  if (ts->scroll_y > max_scroll_y) ts->scroll_y = max_scroll_y;
  if (ts->scroll_y < min_scroll_y) ts->scroll_y = min_scroll_y;

  u8 bg_r = (ts->default_bg >> 16) & 0xff;
  u8 bg_g = (ts->default_bg >> 8) & 0xff;
  u8 bg_b = ts->default_bg & 0xff;

  for (int x = 0; x < 400; x++) {
    u32 col_off = x * 240 * 3;
    for (int y = 0; y < 240; y++) {
      u32 off = col_off + (239 - y) * 3;
      fb[off] = bg_b;
      fb[off+1] = bg_g;
      fb[off+2] = bg_r;
    }
  }

  // 268 MHz system clock, so 268000LL ticks per ms. 500 ms period.
  bool blink_on = (svcGetSystemTick() / (268000LL * 500)) % 2 == 0;

  for (int vy = 0; vy < vis_rows; vy++) {
    int gy = ts->scroll_y + vy;
    int py = vy * char_h;

    for (int vx = 0; vx < vis_cols; vx++) {
      int gx = ts->scroll_x + vx;
      if (gx >= TERM_COLS) break;
      int px = vx * char_w;

      TermCell cell;

      if (gy < 0) {
        // Row is in scrollback history.
        // gy == -1 means the most-recently saved line (newest),
        // gy == -sb_count means the oldest line.
        // Mapping: slot index = (sb_head + sb_count + gy) % TERM_SCROLLBACK
        int age = ts->sb_count + gy; // 0 = oldest, sb_count-1 = newest
        if (age < 0 || age >= ts->sb_count) {
          // Beyond stored history – show blank
          cell.c = ' '; cell.fg = ts->default_fg; cell.bg = ts->default_bg; cell.flags = 0;
        } else {
          int slot = (ts->sb_head + age) % TERM_SCROLLBACK;
          cell = ts->sb_buf[slot][gx];
        }
      } else if (gy < TERM_ROWS) {
        int idx = gy * TERM_COLS + gx;
        cell = ts->grid[idx];
      } else {
        break;
      }

      u8 cell_flags = cell.flags;
      // Only draw cursor when viewing live grid
      if (gy >= 0 && gx == ts->cx && gy == ts->cy && ts->cursor_visible && blink_on) {
        cell_flags ^= TERM_FLAG_REVERSE;
      }

      if (ts->use_5x7) {
        term_draw_char_5x7(fb, px, py, cell.c, cell.fg, cell.bg, ts->zoom_x, ts->zoom_y, cell_flags);
      } else {
        term_draw_char_8x8(fb, px, py, cell.c, cell.fg, cell.bg, ts->zoom_x, ts->zoom_y, cell_flags);
      }
    }
  }
}

#endif
