#ifndef PLATFORM_HID_ASCII_H
#define PLATFORM_HID_ASCII_H

/* HID keyboard usage id -> the bytes a terminal expects.
 *
 * Three of the four consoles that can take a USB keyboard hand over raw usage
 * ids from the HID boot protocol rather than characters, and all three would
 * otherwise grow their own copy of this table. The PS3 is the exception: lv2
 * will do the translation itself, with the user's own layout, so its backend
 * asks for ASCII and never calls in here.
 *
 * US layout, because the usage id alone does not say which layout produced it
 * and there is nowhere on these consoles to ask. A console that knows better -
 * the Wii U hands over a UTF-16 character alongside the usage id - should
 * prefer what it knows and fall back to this only for the keys that have no
 * character at all.
 *
 * Returns a NUL-terminated string in `buf` (at least 8 bytes), or NULL where
 * the key produces nothing: a modifier, or a key this does not carry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Usages 0x04..0x38, the block that produces characters, unshifted then
   shifted. Position is usage - 0x04. */
static const char hid_ascii_lower[] =
  "abcdefghijklmnopqrstuvwxyz"   /* 0x04..0x1D */
  "1234567890"                   /* 0x1E..0x27 */
  "\r\x1b\x7f\t "                /* enter esc backspace tab space */
  "-=[]\\\\;'`,./";              /* 0x2D..0x38, 0x32 doubles as backslash */

static const char hid_ascii_upper[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "!@#$%^&*()"
  "\r\x1b\x7f\t "
  "_+{}||:\"~<>?";

/* Keys whose whole point is the escape sequence they send. Indexed from
   usage 0x49 (Insert) through 0x52 (Up), which is one contiguous run. */
static const char *const hid_nav_seq[] = {
  "\x1b[2~",   /* 0x49 insert   */
  "\x1b[H",    /* 0x4A home     */
  "\x1b[5~",   /* 0x4B page up  */
  "\x1b[3~",   /* 0x4C delete   */
  "\x1b[F",    /* 0x4D end      */
  "\x1b[6~",   /* 0x4E page dn  */
  "\x1b[C",    /* 0x4F right    */
  "\x1b[D",    /* 0x50 left     */
  "\x1b[B",    /* 0x51 down     */
  "\x1b[A",    /* 0x52 up       */
};

/* F1..F12. The first four are the VT100 forms and the rest the xterm ones,
   which is the split every terminfo entry expects. */
static const char *const hid_fkey_seq[] = {
  "\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS",
  "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~",
  "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~",
};

static inline const char *hid_term_bytes(uint8_t usage, bool shift, bool ctrl,
                                         bool caps, char *buf) {
  /* Ctrl first: it rewrites the character block rather than selecting from
     it, and the codes it produces are not in either table. */
  if (ctrl) {
    if (usage >= 0x04 && usage <= 0x1D) {          /* ctrl-a .. ctrl-z */
      buf[0] = (char)(usage - 0x04 + 1); buf[1] = 0; return buf;
    }
    switch (usage) {
      case 0x2F: buf[0] = 0x1b; break;             /* ctrl-[ is escape  */
      case 0x31: buf[0] = 0x1c; break;             /* ctrl-\            */
      case 0x30: buf[0] = 0x1d; break;             /* ctrl-]            */
      case 0x2D: buf[0] = 0x1f; break;             /* ctrl-_            */
      case 0x2C: buf[0] = 0x00; break;             /* ctrl-space is NUL */
      default:   return NULL;
    }
    buf[1] = 0;
    return buf;
  }

  if (usage >= 0x04 && usage <= 0x38) {
    /* Caps lock is letters only - it must not turn 1 into !. */
    bool upper = shift;
    if (usage <= 0x1D && caps) upper = !upper;
    buf[0] = upper ? hid_ascii_upper[usage - 0x04] : hid_ascii_lower[usage - 0x04];
    buf[1] = 0;
    return buf;
  }

  if (usage >= 0x3A && usage <= 0x45) return hid_fkey_seq[usage - 0x3A];
  if (usage >= 0x49 && usage <= 0x52) return hid_nav_seq[usage - 0x49];

  /* The keypad, with num lock assumed on - there is no LED to read back and
     a terminal wants the digits far more than the arrows. */
  switch (usage) {
    case 0x54: buf[0] = '/';  break;
    case 0x55: buf[0] = '*';  break;
    case 0x56: buf[0] = '-';  break;
    case 0x57: buf[0] = '+';  break;
    case 0x58: buf[0] = '\r'; break;
    case 0x62: buf[0] = '0';  break;
    case 0x63: buf[0] = '.';  break;
    default:
      if (usage >= 0x59 && usage <= 0x61) {        /* keypad 1..9 */
        buf[0] = (char)('1' + (usage - 0x59));
        break;
      }
      return NULL;
  }
  buf[1] = 0;
  return buf;
}

#endif /* PLATFORM_HID_ASCII_H */
