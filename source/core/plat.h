#ifndef CORE_PLAT_H
#define CORE_PLAT_H

/* The console interface. Everything under source/core/ talks to the hardware
 * through this header and nothing else - no console SDK header may be included
 * from core/. Each backend under source/platform/<console>/ provides plat.c,
 * plat_cfg.h and plat_hw.h; the -I path in that console's makefile is what
 * selects one.
 *
 * plat_cfg.h supplies the compile-time half: screen and terminal dimensions,
 * SD path prefix, lock size, RAM policy. This header supplies the calls.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "plat_cfg.h"

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

bool plat_init(void);
void plat_exit(void);

/* False once the OS wants the app gone - Horizon's aptMainLoop, wut's
   WHBProcIsRunning, libnx's appletMainLoop. Consoles with no such notion
   return true forever. */
bool plat_running(void);

/* ------------------------------------------------------------------
 * Capabilities
 *
 * Runtime, not #ifdef: core stays one body of code and the settings page can
 * grey out a row rather than the build dropping it. A false here means the
 * console cannot do it at all; a device the *user* turned off is separate and
 * lives in Cfg.
 * ------------------------------------------------------------------ */

typedef struct {
  /* False on single-core consoles (Wii, GameCube), where the emulator has to
     be stepped inline from the main loop instead of owning a thread. */
  bool emu_thread;
  /* False where nothing can point at the panel (GameCube): the on-screen
     keyboard must then be driven by D-pad focus. */
  bool pointer;
  bool net;
  bool rng;          /* a real CSPRNG; false falls back to the tick mixer */
  bool sensors;      /* something to fill plat_sample_axes with           */
  bool audio, camera, mic;
  bool swkbd;        /* a system keyboard applet for the line-compose pane */
  bool speedup;      /* a clock/cache boost was available and taken        */
} plat_caps_t;

const plat_caps_t *plat_caps(void);

/* One line for the boot banner: model, clock, whatever identifies the machine
   a bug report came from. */
const char *plat_model(void);

/* How often the main thread should redraw the terminal and poll input. The
   backend picks these because it knows its own models: where the emulator has
   to share a core, both go coarser, trading UI smoothness for guest
   throughput. */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us);

/* ------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------ */

/* Monotonic microseconds. The guest clock, every timeout in virtio_net.h and
   the UI cadence all derive from this, so it must not jump or wrap during a
   session. */
uint64_t plat_us(void);

/* Milliseconds since the Unix epoch, from a battery-backed clock where the
   console has one. Only rtc_goldfish.h reads this. */
uint64_t plat_wallclock_ms(void);

void plat_sleep_us(uint64_t us);

/* ------------------------------------------------------------------
 * Video
 * ------------------------------------------------------------------ */

/* A drawable rectangle, addressed by signed strides rather than a put_pixel
 * callback: the terminal's inner loop is a per-cell software blit that hoists
 * the column stride out and advances by it, and a call per pixel would undo
 * that. Byte order within a pixel is BGR or BGRA, low byte first.
 *
 * The strides are what absorb the differences. A linear 32bpp screen is
 * {fb, w, h, +4, +w*4}. The 3DS's rotated 24bpp screen is {fb+717, 400, 240,
 * +720, -3} - its framebuffer runs bottom-to-top in columns, so y_stride is
 * negative and base points at the top-left pixel's address, not the
 * allocation's. A backend whose real framebuffer cannot be described this way
 * at all (Wii and GameCube are YUY2, where two pixels share a chroma pair)
 * hands back a linear scratch buffer and converts inside plat_present.
 */
/* Byte order within a pixel, low byte first. Compile-time per console (a
   backend defines PLAT_PIXEL_RGB in its plat_cfg.h if it wants R first), so
   the blitters keep three straight stores and no per-pixel branch. A 32bpp
   surface leaves the fourth byte alone - it is the backend's to fill. */
#ifdef PLAT_PIXEL_RGB
#define PLAT_PX(o, r_, g_, b_) do { (o)[0] = (r_); (o)[1] = (g_); (o)[2] = (b_); } while (0)
#else
#define PLAT_PX(o, r_, g_, b_) do { (o)[0] = (b_); (o)[1] = (g_); (o)[2] = (r_); } while (0)
#endif

typedef struct {
  uint8_t *base;      /* address of pixel (0,0) */
  int      w, h;
  int      x_stride;  /* signed bytes from a pixel to the one at x+1 */
  int      y_stride;  /* signed bytes from a pixel to the one at y+1 */
  int      bpp;       /* 3 or 4 */
} plat_fb_t;

/* Two logical surfaces, not two screens. On the 3DS and Wii U they are the two
   physical panels; on single-screen consoles they are sub-rectangles of one
   framebuffer, which the stride model describes with no special case. */
typedef enum {
  PLAT_SURF_TERM  = 0,   /* the guest's console                     */
  PLAT_SURF_PANEL = 1,   /* touch keyboard and the settings page    */
  PLAT_SURF_COUNT
} plat_surf;

#define PLAT_SURF_BIT(s) (1u << (s))

/* Fills `out` with the surface's current drawable. Valid until the next
   plat_present. */
bool plat_surface(plat_surf s, plat_fb_t *out);

/* Push drawn pixels to the screen. `mask` names which surfaces changed, so a
   backend that can flip them independently need not do both. Blocks for vsync
   where the console has one. */
void plat_present(unsigned mask);

/* ------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------ */

enum {
  PLAT_BTN_QUIT     = 1u << 0,   /* START / PLUS / HOME                    */
  PLAT_BTN_SETTINGS = 1u << 1,   /* SELECT / MINUS                         */
  PLAT_BTN_ZOOM_IN  = 1u << 2,
  PLAT_BTN_ZOOM_OUT = 1u << 3,
  PLAT_BTN_FOLLOW   = 1u << 4,   /* toggle auto-follow of the cursor       */
  PLAT_BTN_FONT     = 1u << 5,   /* toggle 8x8 / 5x7                       */
  PLAT_BTN_UP       = 1u << 6,
  PLAT_BTN_DOWN     = 1u << 7,
  PLAT_BTN_LEFT     = 1u << 8,
  PLAT_BTN_RIGHT    = 1u << 9,
  PLAT_BTN_A        = 1u << 10,
  PLAT_BTN_B        = 1u << 11,
  PLAT_BTN_X        = 1u << 12,
  PLAT_BTN_Y        = 1u << 13,
  PLAT_BTN_SWKBD    = 1u << 14,  /* open the system keyboard applet        */
};

typedef struct {
  uint32_t down;        /* pressed since the previous poll */
  uint32_t held;

  /* Analog stick, deadzone already applied by the backend, ±127 at full
     deflection. Pans the viewport or sends arrow keys, per Cfg.circle_pans. */
  int32_t  pan_x, pan_y;

  /* Panel-local, so the keyboard and settings page never learn where the panel
     sits on the physical screen. ptr_valid is false on a console with no
     pointer at all, and also while the pointer is off-panel (a Wiimote aimed
     away from the sensor bar). */
  bool     ptr_valid;
  bool     ptr_down;
  bool     ptr_tapped;  /* pressed since the previous poll */
  int16_t  ptr_x, ptr_y;
} plat_input_t;

/* Samples the console's input hardware. Everything that reads a cached HID
   state must run on whichever thread calls this. */
void plat_poll_input(plat_input_t *out);

/* ------------------------------------------------------------------
 * Threads
 *
 * Exactly one thread is ever created (the emulator's), so there is no handle
 * to pass around. plat_thread_start returns false where caps.emu_thread is
 * false, or where the console had a core but refused it.
 * ------------------------------------------------------------------ */

bool plat_thread_start(void (*entry)(void *), void *arg);
void plat_thread_join(void);

/* Which core the emulator actually landed on, for the boot banner. Only
   meaningful after a successful plat_thread_start. */
const char *plat_thread_desc(void);

/* Sized in plat_cfg.h; plat.c static_asserts the real lock fits. Opaque so
   core needs no console SDK header to declare one. */
typedef union {
  uint64_t      _align;
  unsigned char opaque[PLAT_MUTEX_SIZE];
} plat_mutex_t;

void plat_mutex_init(plat_mutex_t *m);
void plat_mutex_lock(plat_mutex_t *m);
void plat_mutex_unlock(plat_mutex_t *m);

/* ------------------------------------------------------------------
 * Guest memory byte order
 *
 * The guest is little-endian RISC-V. On a big-endian host - every PowerPC
 * console here - each 16- and 32-bit access to guest memory has to be
 * reversed, or an instruction fetch returns its own bytes backwards and the
 * machine executes noise. PowerPC has load/store-byte-reversed instructions,
 * so GCC compiles these to one instruction rather than a shift-and-or chain.
 * On a little-endian host they vanish entirely.
 * ------------------------------------------------------------------ */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PLAT_BIG_ENDIAN 1
#define plat_le16(v) __builtin_bswap16((uint16_t)(v))
#define plat_le32(v) __builtin_bswap32((uint32_t)(v))
#define plat_le64(v) __builtin_bswap64((uint64_t)(v))
#else
#define PLAT_BIG_ENDIAN 0
#define plat_le16(v) (v)
#define plat_le32(v) (v)
#define plat_le64(v) (v)
#endif

/* Accessors for the little-endian structures the guest and the virtio devices
   share. Neither guest loads nor virtqueue fields are reliably aligned.

   On a big-endian host the bytes are assembled by hand rather than swapped
   after a word load. PowerPC's byte-reversed instructions (lwbrx/stwbrx) are
   the obvious lowering and GCC will pick them, but they raise an alignment
   exception on an unaligned address - which a guest doing an unaligned store
   promptly produces. Byte access costs a few instructions and cannot trap. */
#if PLAT_BIG_ENDIAN
/* Byte by byte through volatile pointers. The obvious lowering - PowerPC's
   lwbrx/stwbrx - is what the volatile is there to prevent: those instructions
   raise an alignment exception on an unaligned address, and Cemu does not
   appear to implement them at all, taking the host down instead. */
static inline uint16_t g_ld16(const void *p) {
  const volatile uint8_t *b = (const volatile uint8_t *)p;
  return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t g_ld32(const void *p) {
  const volatile uint8_t *b = (const volatile uint8_t *)p;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t g_ld64(const void *p) {
  return (uint64_t)g_ld32(p) | ((uint64_t)g_ld32((const uint8_t *)p + 4) << 32);
}
static inline void g_st16(void *p, uint16_t v) {
  volatile uint8_t *b = (volatile uint8_t *)p;
  b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
}
static inline void g_st32(void *p, uint32_t v) {
  volatile uint8_t *b = (volatile uint8_t *)p;
  b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static inline void g_st64(void *p, uint64_t v) {
  g_st32(p, (uint32_t)v);
  g_st32((uint8_t *)p + 4, (uint32_t)(v >> 32));
}
#else
static inline uint16_t g_ld16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t g_ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t g_ld64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void g_st16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void g_st32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void g_st64(void *p, uint64_t v) { memcpy(p, &v, 8); }
#endif

/* Raised by a backend whose console suspended the process - a modal system
   keyboard applet, a HOME menu overlay - and cleared by the emulation thread.
   Without it the guest is handed the whole suspended interval as one step,
   which fires a backlog of timer interrupts and an RCU stall splat. Defined by
   core; see EmuStepBatch. */
extern volatile bool g_emu_rebase_clock;

/* ------------------------------------------------------------------
 * Panel keyboard
 *
 * Which keyboard the panel shows is the console's business: a touch grid, a
 * pointer-driven one, a D-pad-focused one where nothing can point, or the
 * system's own applet. Core only opens the panel, forwards input and asks for
 * a repaint; the backend decides what appears there and pushes the bytes it
 * produces straight into the ring below.
 * ------------------------------------------------------------------ */

/* The guest's keyboard input ring, provided by core. A backend's keyboard
   pushes into it rather than returning bytes, because one gesture can produce
   a whole line (a system applet) or none at all (a modifier). */
void rx_push(char c);
void rx_push_str(const char *s);

struct AdaPalette;

void pkbd_init(void);
/* Restyle and reconfigure from live settings. Safe to call while the guest
   runs; the next pkbd_draw repaints. */
void pkbd_apply(const struct AdaPalette *p, bool backspace_del, bool shift_oneshot,
                int mode);
/* The panel was painted over by something else (the settings page), so the
   next draw must be a full repaint rather than a dirty-tracked one. */
void pkbd_invalidate(void);
void pkbd_update(const plat_input_t *in);
void pkbd_draw(void);

/* ------------------------------------------------------------------
 * Networking
 *
 * virtio_net.h is a userspace NAT over ordinary BSD sockets; only bringing the
 * stack up differs per console. Where the socket calls themselves are spelled
 * differently (libogc prefixes them net_*), plat_sock.h maps them back.
 * ------------------------------------------------------------------ */

bool plat_net_init(void);
void plat_net_exit(void);

/* ------------------------------------------------------------------
 * Entropy
 * ------------------------------------------------------------------ */

/* False when the console has no CSPRNG, leaving virtio_rng.h to fall back. */
bool plat_random(void *buf, size_t len);

/* ------------------------------------------------------------------
 * Sensors
 * ------------------------------------------------------------------ */

/* How many axes plat_sample_axes fills: accelerometer x/y/z, gyroscope x/y/z,
   then two console-specific analog controls. virtio_input.h turns them into an
   evdev node whose shape is the same everywhere; a console without a given
   axis reports 0 rather than the axis disappearing. */
#define VI_NAXES 8

/* Fills VI_NAXES entries with the console's motion and
   slider readings. Axes the hardware does not have read 0. Must be called from
   the same thread as plat_poll_input. */
void plat_sample_axes(int32_t *out);

/* ------------------------------------------------------------------
 * 9P passthrough trees
 * ------------------------------------------------------------------ */

typedef struct {
  const char *aname;  /* what `-o aname=` mounts, and the subdirectory name */
  const char *root;   /* host path prefix, e.g. "sdmc:/"                    */
  bool        ro;     /* enforced server-side, not by asking the guest      */
} plat_tree_t;

/* The real-filesystem trees this console exports, in aname order. The
   synthetic hw/ tree is not among them - it has no host path. */
int plat_v9p_trees(const plat_tree_t **out);

/* Make tree `idx` reachable through ordinary POSIX calls on its root path
   (mounting an archive, bringing up libfat). False means the tree is simply
   absent and the guest's mount of it fails cleanly. */
bool plat_v9p_mount(int idx);
void plat_v9p_unmount_all(void);

/* Free bytes on the writable tree, for the guest's statfs. -1 where the
   console cannot report it. */
int64_t plat_v9p_free_bytes(void);

/* ------------------------------------------------------------------
 * Synthetic hw/ tree
 *
 * The table itself lives in each backend's plat_hw.h, since both its rows and
 * its handlers are console-specific. Core only needs the row type, the stream
 * kinds, and the three streaming calls below.
 * ------------------------------------------------------------------ */

enum { HWS_NONE = 0, HWS_CAM_OUT, HWS_CAM_IN, HWS_MIC, HWS_AUDIO };

typedef struct {
  const char *name;
  uint32_t    perm;                     /* permission bits only */
  int       (*rd_text)(char *b, int n);
  int       (*wr)(const char *b, int len);
  int         stream;                   /* HWS_* */
} plat_hw_ent;

/* Defined by the backend, in plat.c. Ordering is the order the guest sees in
   a directory listing. */
extern const plat_hw_ent plat_hw_files[];
extern const int         plat_hw_count;

/* Camera: grabs one frame into a buffer the callee allocates, returning its
   length, or 0 if the capture failed. `inner` picks the user-facing sensor on
   a console that has two. Caller frees. */
int plat_hw_camera(bool inner, uint8_t **frame);

/* Stream semantics: returns whatever arrived since the previous call, so the
   file offset is ignored. Never returns 0 while the mic is live - a
   zero-length read is EOF to every POSIX reader. */
int plat_hw_mic_read(uint8_t *out, int max);

/* Signed 16-bit stereo PCM. Never blocks: this runs on the emulation thread,
   so waiting on the DSP would freeze the guest. Returns len even when buffers
   were dropped, since a short write makes guest userspace spin. */
int plat_hw_audio_write(const uint8_t *data, int len);

#endif /* CORE_PLAT_H */
