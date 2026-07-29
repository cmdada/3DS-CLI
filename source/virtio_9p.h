#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <3ds.h>
#include "hw3ds.h"

/*
 * Virtio-9p device: 3DS-side filesystem passthrough.
 *
 * One virtio-9p device (mount tag "3ds") exporting a synthetic root whose
 * subdirectories are the four things worth reaching:
 *
 *   sd/    the real SD card (sdmc:/), read-write
 *   nand/  the CTR NAND filesystem, read-only
 *   twl/   the TWL NAND filesystem, read-only
 *   hw/    synthetic sensor/camera/mic/audio files (hw3ds.h)
 *
 * So a single `mount -t 9p ... 3ds /mnt/3ds` brings up all of them. They are
 * subdirectories rather than four separate mounts because Linux's virtio-9p
 * transport ties one channel to one mount — a second mount of the same
 * device fails with "no channels available" no matter what aname= says, and
 * giving each tree its own virtio device would burn four MMIO windows and
 * four interrupt lines for what is really one filesystem. `aname=<tree>`
 * still works for mounting a single subtree by itself.
 *
 * 9P rather than a block device because what the guest wants here is the
 * host's *files*, not its partitions. Handing over the SD card as raw
 * blocks would mean the guest reimplementing FAT while Horizon has the
 * same card mounted underneath — two writers on one filesystem, which
 * corrupts it. Going through 9P means every access is a real fs call on
 * the 3DS side, so the console and the guest stay coherent.
 *
 * The dialect is 9P2000.L (the Linux one): its messages map almost 1:1
 * onto POSIX calls, so there's no Tstat/mode-bit translation layer of the
 * sort plain 9P2000 would need.
 *
 * The NAND trees are read-only on purpose and enforced here, in the server,
 * rather than by asking the guest to mount with -o ro: a guest that ignores
 * the flag would otherwise be writing to system files with no filesystem
 * driver on the 3DS side to keep Horizon's own view consistent. They're
 * also simply absent unless the app is running with enough permission to
 * mount them (Luma3DS-style extended homebrew perms) — the tree just fails
 * to attach, and the guest's mount fails cleanly.
 *
 * Everything runs synchronously on the emulation thread, exactly like
 * virtio_blk.h: requests are serviced inside the QUEUE_NOTIFY store.
 */

#define VIRTIO_9P_BASE   0x10006000u
#define VIRTIO_9P_SIZE   0x1000u
#define V9P_MOUNT_TAG    "3ds"

/* Bounded by what the 3DS can spare, not by the protocol. 16K keeps a
   whole Tread's payload in one round trip while costing two static
   buffers; the guest is told this value at Tversion and clamps to it. */
#define V9P_MSIZE        16384u
#define V9P_PATH_MAX     512
#define V9P_MAX_FIDS     64

/* virtio-9p feature bit 0. The Linux driver fetches the mount tag with
   virtio_cread_feature(), which hard-fails with -ENOENT if this isn't
   offered, so the device would never probe without it. */
#define VIRTIO_9P_F_MOUNT_TAG 0x1u

/* 9P2000.L message types */
#define P9_RLERROR     7
#define P9_TSTATFS     8
#define P9_RSTATFS     9
#define P9_TLOPEN      12
#define P9_RLOPEN      13
#define P9_TLCREATE    14
#define P9_RLCREATE    15
#define P9_TSYMLINK    16
#define P9_TMKNOD      18
#define P9_TRENAME     20
#define P9_RRENAME     21
#define P9_TREADLINK   22
#define P9_TGETATTR    24
#define P9_RGETATTR    25
#define P9_TSETATTR    26
#define P9_RSETATTR    27
#define P9_TXATTRWALK  30
#define P9_TXATTRCREATE 32
#define P9_TREADDIR    40
#define P9_RREADDIR    41
#define P9_TFSYNC      50
#define P9_RFSYNC      51
#define P9_TLOCK       52
#define P9_RLOCK       53
#define P9_TGETLOCK    54
#define P9_RGETLOCK    55
#define P9_TLINK       70
#define P9_RLINK       71
#define P9_TMKDIR      72
#define P9_RMKDIR      73
#define P9_TRENAMEAT   74
#define P9_RRENAMEAT   75
#define P9_TUNLINKAT   76
#define P9_RUNLINKAT   77
#define P9_TVERSION    100
#define P9_RVERSION    101
#define P9_TATTACH     104
#define P9_RATTACH     105
#define P9_TFLUSH      108
#define P9_RFLUSH      109
#define P9_TWALK       110
#define P9_RWALK       111
#define P9_TREAD       116
#define P9_RREAD       117
#define P9_TWRITE      118
#define P9_RWRITE      119
#define P9_TCLUNK      120
#define P9_RCLUNK      121
#define P9_TREMOVE     122
#define P9_RREMOVE     123

/* QID types */
#define P9_QTDIR   0x80
#define P9_QTFILE  0x00

/* Linux errno values. Spelled out rather than taken from newlib's errno.h
   because these travel to a Linux guest and several of newlib's numbers
   disagree with Linux's (ENOTEMPTY is 90 here and 39 there). */
#define L_EPERM        1
#define L_ENOENT       2
#define L_EIO          5
#define L_EBADF        9
#define L_EACCES      13
#define L_EEXIST      17
#define L_ENOTDIR     20
#define L_EISDIR      21
#define L_EINVAL      22
#define L_ENFILE      23
#define L_ENOSPC      28
#define L_EROFS       30
#define L_ENOSYS      38
#define L_ENOTEMPTY   39
#define L_ENODATA     61
#define L_EOPNOTSUPP  95

/* Linux open flags (guest-side values, again not newlib's) */
#define L_O_WRONLY     01
#define L_O_RDWR       02
#define L_O_CREAT      0100
#define L_O_EXCL       0200
#define L_O_TRUNC      01000
#define L_O_APPEND     02000
#define L_O_DIRECTORY  0200000

/* The four exports, plus a synthetic parent directory that contains them.
   V9P_TREE_ROOT is what an ordinary mount lands on, so the whole set costs
   exactly one mount and one virtio channel: Linux's virtio-9p transport
   binds a channel to a single mount (p9_virtio_create refuses a second with
   "no channels available"), so four separate `aname=` mounts of one device
   cannot work — only the first would succeed. Exporting them as
   subdirectories of one tree sidesteps that entirely, and still leaves
   aname= usable for mounting a single subtree on its own. */
enum { V9P_TREE_SD = 0, V9P_TREE_NAND, V9P_TREE_TWL, V9P_TREE_HW,
       V9P_TREE_COUNT, V9P_TREE_ROOT = V9P_TREE_COUNT, V9P_TREE_TOTAL };

static const char *const v9p_tree_aname[V9P_TREE_TOTAL] =
    { "sd", "nand", "twl", "hw", "/" };
static const char *const v9p_tree_root[V9P_TREE_TOTAL]  =
    { "sdmc:/", "v9nand:/", "v9twl:/", "/", "/" };
/* The root listing is synthetic, so it's read-only like the NAND trees. */
static const bool        v9p_tree_ro[V9P_TREE_TOTAL]    =
    { false, true, true, false, true };
static bool              v9p_tree_ok[V9P_TREE_TOTAL];

/* ------------------------------------------------------------------
 * Synthetic `hw` tree
 * ------------------------------------------------------------------ */

enum { HWS_NONE = 0, HWS_CAM_OUT, HWS_CAM_IN, HWS_MIC, HWS_AUDIO };

typedef struct {
    const char *name;
    uint32_t    perm;                        /* permission bits only */
    int       (*rd_text)(char *b, int n);
    int       (*wr)(const char *b, int len);
    int         stream;
} v9p_hw_ent;

static const v9p_hw_ent v9p_hw_files[] = {
    { "info",             0444, hw_rd_info,           NULL,        HWS_NONE },
    { "battery",          0444, hw_rd_battery,        NULL,        HWS_NONE },
    { "battery_voltage",  0444, hw_rd_battery_voltage,NULL,        HWS_NONE },
    { "charging",         0444, hw_rd_charging,       NULL,        HWS_NONE },
    { "adapter",          0444, hw_rd_adapter,        NULL,        HWS_NONE },
    { "shell",            0444, hw_rd_shell,          NULL,        HWS_NONE },
    { "steps",            0444, hw_rd_steps,          NULL,        HWS_NONE },
    { "wifi",             0444, hw_rd_wifi,           NULL,        HWS_NONE },
    { "accel",            0444, hw_rd_accel,          NULL,        HWS_NONE },
    { "gyro",             0444, hw_rd_gyro,           NULL,        HWS_NONE },
    { "slider_3d",        0444, hw_rd_slider_3d,      NULL,        HWS_NONE },
    { "slider_volume",    0444, hw_rd_slider_volume,  NULL,        HWS_NONE },
    { "model",            0444, hw_rd_model,          NULL,        HWS_NONE },
    { "region",           0444, hw_rd_region,         NULL,        HWS_NONE },
    { "language",         0444, hw_rd_language,       NULL,        HWS_NONE },
    { "firmware",         0444, hw_rd_firmware,       NULL,        HWS_NONE },
    { "leds",             0222, NULL,                 hw_wr_leds,  HWS_NONE },
    { "camera_outer.rgb565", 0444, NULL,              NULL,        HWS_CAM_OUT },
    { "camera_inner.rgb565", 0444, NULL,              NULL,        HWS_CAM_IN  },
    { "mic.pcm",          0444, NULL,                 NULL,        HWS_MIC   },
    { "audio.pcm",        0222, NULL,                 NULL,        HWS_AUDIO },
};
#define V9P_HW_COUNT ((int)(sizeof(v9p_hw_files)/sizeof(v9p_hw_files[0])))

static int v9p_hw_lookup(const char *name) {
    for (int i = 0; i < V9P_HW_COUNT; i++)
        if (!strcmp(v9p_hw_files[i].name, name)) return i;
    return -1;
}

/* ------------------------------------------------------------------
 * Fids
 * ------------------------------------------------------------------ */

typedef struct {
    bool     used;
    uint32_t fid;
    int      tree;
    char     path[V9P_PATH_MAX];
    bool     is_dir;
    int      hw_idx;        /* index into v9p_hw_files, or -1 */
    FILE    *fp;
    DIR     *dp;
    uint64_t dir_pos;       /* cookie of the next entry readdir will return */
    uint8_t *snap;          /* snapshot of a synthetic file, taken at open */
    uint32_t snap_len;
} v9p_fid;

/* Opened once at init purely so Tstatfs can ask for free bytes without
   paying for an archive open on every call. */
static FS_Archive v9p_sdmc_archive;
static bool       v9p_sdmc_ok;

static struct {
    uint32_t dev_feat_sel, drv_feat_sel;
    uint32_t queue_num, queue_ready, int_status, status;
    uint32_t queue_desc_lo, queue_driver_lo, queue_device_lo;
    uint16_t last_avail_idx;
    uint32_t msize;
    v9p_fid  fids[V9P_MAX_FIDS];
} v9p;

static v9p_fid *v9p_fid_find(uint32_t fid) {
    for (int i = 0; i < V9P_MAX_FIDS; i++)
        if (v9p.fids[i].used && v9p.fids[i].fid == fid) return &v9p.fids[i];
    return NULL;
}

static v9p_fid *v9p_fid_alloc(uint32_t fid) {
    v9p_fid *e = v9p_fid_find(fid);
    if (e) return e;
    for (int i = 0; i < V9P_MAX_FIDS; i++) {
        if (!v9p.fids[i].used) {
            memset(&v9p.fids[i], 0, sizeof(v9p_fid));
            v9p.fids[i].used = true;
            v9p.fids[i].fid  = fid;
            v9p.fids[i].hw_idx = -1;
            return &v9p.fids[i];
        }
    }
    return NULL;
}

static void v9p_fid_close_handles(v9p_fid *e) {
    if (e->fp) { fclose(e->fp); e->fp = NULL; }
    if (e->dp) { closedir(e->dp); e->dp = NULL; }
    if (e->snap) { free(e->snap); e->snap = NULL; }
    e->snap_len = 0;
}

static void v9p_fid_free(v9p_fid *e) {
    v9p_fid_close_handles(e);
    e->used = false;
}

/* ------------------------------------------------------------------
 * Path helpers
 * ------------------------------------------------------------------ */

static bool v9p_name_ok(const char *n) {
    /* "." and ".." are handled by the walk loop itself; a literal slash or
       an embedded ".." arriving as a single name component would escape the
       tree root, so those are rejected outright. */
    if (!*n || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    return strchr(n, '/') == NULL && strchr(n, '\\') == NULL;
}

static bool v9p_path_push(char *path, const char *name) {
    size_t l = strlen(path);
    size_t n = strlen(name);
    bool need_slash = (l > 0 && path[l - 1] != '/');
    if (l + (need_slash ? 1 : 0) + n + 1 > V9P_PATH_MAX) return false;
    if (need_slash) path[l++] = '/';
    memcpy(path + l, name, n + 1);
    return true;
}

static void v9p_path_pop(int tree, char *path) {
    size_t rootlen = strlen(v9p_tree_root[tree]);
    size_t l = strlen(path);
    if (l <= rootlen) return;               /* already at the root */
    while (l > rootlen && path[l - 1] != '/') l--;
    if (l > rootlen) l--;                   /* drop the separator itself */
    if (l < rootlen) l = rootlen;
    path[l] = 0;
}

static bool v9p_is_tree_root(int tree, const char *path) {
    return strlen(path) <= strlen(v9p_tree_root[tree]);
}

/* The tree index is folded in because paths alone are not unique across
   trees: the synthetic root and the hw tree are both "/", and the guest
   kernel keys its inode cache on this value — colliding qids would make it
   serve one directory's contents for the other. */
static uint64_t v9p_qid_path(int tree, const char *s) {
    uint64_t h = 1469598103934665603ull;      /* FNV-1a */
    h ^= (uint8_t)('0' + tree); h *= 1099511628211ull;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

/* ------------------------------------------------------------------
 * Message readers / writers
 * ------------------------------------------------------------------ */

typedef struct { const uint8_t *b; uint32_t len, pos; bool err; } v9p_rd;
typedef struct { uint8_t *b; uint32_t cap, len; bool ovf; } v9p_wr;

static uint8_t rd8(v9p_rd *r) {
    if (r->pos + 1 > r->len) { r->err = true; return 0; }
    return r->b[r->pos++];
}
static uint16_t rd16(v9p_rd *r) {
    if (r->pos + 2 > r->len) { r->err = true; return 0; }
    uint16_t v; memcpy(&v, r->b + r->pos, 2); r->pos += 2; return v;
}
static uint32_t rd32(v9p_rd *r) {
    if (r->pos + 4 > r->len) { r->err = true; return 0; }
    uint32_t v; memcpy(&v, r->b + r->pos, 4); r->pos += 4; return v;
}
static uint64_t rd64(v9p_rd *r) {
    if (r->pos + 8 > r->len) { r->err = true; return 0; }
    uint64_t v; memcpy(&v, r->b + r->pos, 8); r->pos += 8; return v;
}
static void rdstr(v9p_rd *r, char *out, size_t outsz) {
    uint16_t n = rd16(r);
    if (r->err || r->pos + n > r->len) { r->err = true; out[0] = 0; return; }
    size_t c = n < outsz - 1 ? n : outsz - 1;
    memcpy(out, r->b + r->pos, c);
    out[c] = 0;
    r->pos += n;
}

static void wr8(v9p_wr *w, uint8_t v) {
    if (w->len + 1 > w->cap) { w->ovf = true; return; }
    w->b[w->len++] = v;
}
static void wr16(v9p_wr *w, uint16_t v) {
    if (w->len + 2 > w->cap) { w->ovf = true; return; }
    memcpy(w->b + w->len, &v, 2); w->len += 2;
}
static void wr32(v9p_wr *w, uint32_t v) {
    if (w->len + 4 > w->cap) { w->ovf = true; return; }
    memcpy(w->b + w->len, &v, 4); w->len += 4;
}
static void wr64(v9p_wr *w, uint64_t v) {
    if (w->len + 8 > w->cap) { w->ovf = true; return; }
    memcpy(w->b + w->len, &v, 8); w->len += 8;
}
static void wrbytes(v9p_wr *w, const void *p, uint32_t n) {
    if (w->len + n > w->cap) { w->ovf = true; return; }
    memcpy(w->b + w->len, p, n); w->len += n;
}
static void wrstr(v9p_wr *w, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    wr16(w, (uint16_t)n);
    wrbytes(w, s, n);
}
static void wrqid(v9p_wr *w, uint8_t type, uint64_t path) {
    wr8(w, type);
    wr32(w, 0);          /* version - unused, we don't cache by it */
    wr64(w, path);
}

/* Every reply starts with size[4] type[1] tag[2]; size is backfilled once
   the body is complete. */
static void v9p_begin(v9p_wr *w, uint8_t type, uint16_t tag) {
    w->len = 0; w->ovf = false;
    wr32(w, 0);
    wr8(w, type);
    wr16(w, tag);
}
static uint32_t v9p_finish(v9p_wr *w) {
    uint32_t sz = w->len;
    memcpy(w->b, &sz, 4);
    return sz;
}
static uint32_t v9p_error(v9p_wr *w, uint16_t tag, uint32_t ecode) {
    v9p_begin(w, P9_RLERROR, tag);
    wr32(w, ecode);
    return v9p_finish(w);
}

/* ------------------------------------------------------------------
 * stat helpers
 * ------------------------------------------------------------------ */

static bool v9p_stat_path(v9p_fid *e, struct stat *st) {
    memset(st, 0, sizeof(*st));
    if (e->tree == V9P_TREE_ROOT) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return true;
    }
    if (e->tree == V9P_TREE_HW) {
        if (e->hw_idx < 0) {
            st->st_mode = S_IFDIR | 0555;
            st->st_nlink = 2;
        } else {
            st->st_mode = S_IFREG | v9p_hw_files[e->hw_idx].perm;
            st->st_nlink = 1;
            /* Synthetic files have no meaningful length until they're read;
               claiming a fixed size would make `cat` stop early or pad with
               zeroes, so they report 0 and rely on the short read at EOF. */
            st->st_size = 0;
        }
        return true;
    }
    return stat(e->path, st) == 0;
}

static uint8_t v9p_qid_type(const struct stat *st) {
    return S_ISDIR(st->st_mode) ? P9_QTDIR : P9_QTFILE;
}

/* ------------------------------------------------------------------
 * hw file open / read / write
 * ------------------------------------------------------------------ */

static uint32_t v9p_hw_open(v9p_fid *e) {
    const v9p_hw_ent *h = &v9p_hw_files[e->hw_idx];

    if (h->rd_text) {
        /* Snapshot at open so that a multi-read `cat` sees one coherent
           value rather than re-sampling the sensor per read() and
           returning a spliced-together number. */
        char tmp[512];
        int n = h->rd_text(tmp, sizeof(tmp));
        if (n < 0) n = 0;
        if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
        e->snap = (uint8_t *)malloc((size_t)n + 1);
        if (!e->snap) return L_EIO;
        memcpy(e->snap, tmp, (size_t)n);
        e->snap_len = (uint32_t)n;
        return 0;
    }

    if (h->stream == HWS_CAM_OUT || h->stream == HWS_CAM_IN) {
        /* One frame is grabbed per open; reads then walk that frame. The
           guest reopens to get a newer one. */
        e->snap = (uint8_t *)malloc(HW_CAM_BYTES);
        if (!e->snap) return L_EIO;
        u32 sel  = (h->stream == HWS_CAM_OUT) ? SELECT_OUT1 : SELECT_IN1;
        u32 port = PORT_CAM1;
        int got = hw_capture_frame(sel, port, e->snap);
        if (got <= 0) { free(e->snap); e->snap = NULL; return L_EIO; }
        e->snap_len = (uint32_t)got;
        return 0;
    }

    /* Start capture at open rather than on the first read, so the ring has
       had a moment to fill by the time the guest actually reads. */
    if (h->stream == HWS_MIC) hw_mic_start();

    return 0; /* mic/audio/leds are streams - nothing to snapshot */
}

/* ------------------------------------------------------------------
 * Request handling
 * ------------------------------------------------------------------ */

static uint32_t v9p_handle(const uint8_t *req, uint32_t reqlen,
                           uint8_t *resp, uint32_t respcap) {
    v9p_rd r = { req, reqlen, 0, false };
    v9p_wr w = { resp, respcap, 0, false };

    uint32_t msgsize = rd32(&r);
    uint8_t  type    = rd8(&r);
    uint16_t tag     = rd16(&r);
    (void)msgsize;
    if (r.err) return 0;

    switch (type) {

    case P9_TVERSION: {
        uint32_t ms = rd32(&r);
        char ver[32];
        rdstr(&r, ver, sizeof(ver));
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        if (ms < V9P_MSIZE) v9p.msize = ms; else v9p.msize = V9P_MSIZE;
        /* Tversion also resets all protocol state, by spec. */
        for (int i = 0; i < V9P_MAX_FIDS; i++)
            if (v9p.fids[i].used) v9p_fid_free(&v9p.fids[i]);
        v9p_begin(&w, P9_RVERSION, tag);
        wr32(&w, v9p.msize);
        /* Anything but 9P2000.L gets the "unknown" reply, which makes the
           client either downgrade or give up rather than mis-parse. */
        wrstr(&w, strcmp(ver, "9P2000.L") == 0 ? "9P2000.L" : "unknown");
        return v9p_finish(&w);
    }

    case P9_TATTACH: {
        uint32_t fid = rd32(&r);
        rd32(&r);                        /* afid - no auth */
        char uname[64], aname[64];
        rdstr(&r, uname, sizeof(uname));
        rdstr(&r, aname, sizeof(aname));
        if (r.err) return v9p_error(&w, tag, L_EINVAL);

        /* No aname (or "/") mounts the parent directory holding every
           available tree; naming one mounts just that subtree. */
        int tree = -1;
        if (!aname[0] || !strcmp(aname, "/")) tree = V9P_TREE_ROOT;
        else for (int i = 0; i < V9P_TREE_COUNT; i++)
            if (!strcmp(aname, v9p_tree_aname[i])) { tree = i; break; }
        if (tree < 0) return v9p_error(&w, tag, L_ENOENT);
        if (!v9p_tree_ok[tree]) return v9p_error(&w, tag, L_ENODATA);

        v9p_fid *e = v9p_fid_alloc(fid);
        if (!e) return v9p_error(&w, tag, L_ENFILE);
        e->tree = tree;
        e->hw_idx = -1;
        e->is_dir = true;
        snprintf(e->path, V9P_PATH_MAX, "%s", v9p_tree_root[tree]);

        v9p_begin(&w, P9_RATTACH, tag);
        wrqid(&w, P9_QTDIR, v9p_qid_path(e->tree, e->path));
        return v9p_finish(&w);
    }

    case P9_TWALK: {
        uint32_t fid = rd32(&r), newfid = rd32(&r);
        uint16_t nwname = rd16(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);

        /* Walk against a scratch copy so a failure part-way leaves both the
           original fid and newfid untouched, as the spec requires. */
        int      wtree = e->tree;
        char     wpath[V9P_PATH_MAX];
        int      whw   = e->hw_idx;
        snprintf(wpath, sizeof(wpath), "%s", e->path);

        uint8_t  qtypes[16];
        uint64_t qpaths[16];
        int      nq = 0;
        uint32_t err = 0;

        if (nwname > 16) return v9p_error(&w, tag, L_EINVAL);

        for (int i = 0; i < nwname; i++) {
            char name[256];
            rdstr(&r, name, sizeof(name));
            if (r.err) return v9p_error(&w, tag, L_EINVAL);

            if (!strcmp(name, ".")) {
                /* stay put */
            } else if (!strcmp(name, "..")) {
                /* Climbing out of a tree's own root lands in the synthetic
                   parent, so the whole export walks like one filesystem. */
                if (wtree == V9P_TREE_ROOT) {
                    /* already at the top */
                } else if (wtree == V9P_TREE_HW) {
                    if (whw >= 0) { whw = -1; snprintf(wpath, sizeof(wpath), "/"); }
                    else wtree = V9P_TREE_ROOT;
                } else if (v9p_is_tree_root(wtree, wpath)) {
                    wtree = V9P_TREE_ROOT;
                    snprintf(wpath, sizeof(wpath), "/");
                } else {
                    v9p_path_pop(wtree, wpath);
                }
            } else if (!v9p_name_ok(name)) {
                err = L_ENOENT; break;
            } else if (wtree == V9P_TREE_ROOT) {
                int idx = -1;
                for (int t = 0; t < V9P_TREE_COUNT; t++)
                    if (v9p_tree_ok[t] && !strcmp(name, v9p_tree_aname[t])) { idx = t; break; }
                if (idx < 0) { err = L_ENOENT; break; }
                wtree = idx;
                whw = -1;
                snprintf(wpath, sizeof(wpath), "%s", v9p_tree_root[idx]);
            } else if (wtree == V9P_TREE_HW) {
                if (whw >= 0) { err = L_ENOTDIR; break; }
                int idx = v9p_hw_lookup(name);
                if (idx < 0) { err = L_ENOENT; break; }
                whw = idx;
                snprintf(wpath, sizeof(wpath), "/%s", name);
            } else {
                if (!v9p_path_push(wpath, name)) { err = L_ENOENT; break; }
                struct stat st;
                if (stat(wpath, &st) != 0) { err = L_ENOENT; break; }
            }

            /* Record the qid of each component reached so far. */
            if (wtree == V9P_TREE_ROOT) {
                qtypes[nq] = P9_QTDIR;
            } else if (wtree == V9P_TREE_HW) {
                qtypes[nq] = (whw < 0) ? P9_QTDIR : P9_QTFILE;
            } else {
                struct stat st;
                if (stat(wpath, &st) != 0) { err = L_ENOENT; break; }
                qtypes[nq] = v9p_qid_type(&st);
            }
            qpaths[nq] = v9p_qid_path(wtree, wpath);
            nq++;
        }

        /* A walk that fails on the very first component is an error; one
           that fails later returns the qids it managed, and the client
           retries the rest. */
        if (err && nq == 0) return v9p_error(&w, tag, err);

        if (nq == nwname) {
            v9p_fid *ne = (newfid == fid) ? e : v9p_fid_alloc(newfid);
            if (!ne) return v9p_error(&w, tag, L_ENFILE);
            if (ne != e) {
                v9p_fid_close_handles(ne);
                ne->hw_idx = -1;
            }
            ne->tree   = wtree;
            ne->hw_idx = whw;
            snprintf(ne->path, V9P_PATH_MAX, "%s", wpath);
            if (wtree == V9P_TREE_ROOT) ne->is_dir = true;
            else if (wtree == V9P_TREE_HW) ne->is_dir = (whw < 0);
            else {
                struct stat st;
                ne->is_dir = (stat(wpath, &st) == 0) && S_ISDIR(st.st_mode);
            }
        }

        v9p_begin(&w, P9_RWALK, tag);
        wr16(&w, (uint16_t)nq);
        for (int i = 0; i < nq; i++) wrqid(&w, qtypes[i], qpaths[i]);
        return v9p_finish(&w);
    }

    case P9_TGETATTR: {
        uint32_t fid = rd32(&r);
        uint64_t valid = rd64(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        struct stat st;
        if (!v9p_stat_path(e, &st)) return v9p_error(&w, tag, L_ENOENT);

        v9p_begin(&w, P9_RGETATTR, tag);
        wr64(&w, valid);                                  /* we answer all of it */
        wrqid(&w, v9p_qid_type(&st), v9p_qid_path(e->tree, e->path));
        wr32(&w, (uint32_t)st.st_mode);
        wr32(&w, 0);                                      /* uid: everything is root */
        wr32(&w, 0);                                      /* gid */
        wr64(&w, st.st_nlink ? (uint64_t)st.st_nlink : 1);
        wr64(&w, 0);                                      /* rdev */
        wr64(&w, (uint64_t)st.st_size);
        wr64(&w, 512);                                    /* blksize */
        wr64(&w, (uint64_t)((st.st_size + 511) / 512));   /* blocks */
        wr64(&w, (uint64_t)st.st_atime); wr64(&w, 0);
        wr64(&w, (uint64_t)st.st_mtime); wr64(&w, 0);
        wr64(&w, (uint64_t)st.st_ctime); wr64(&w, 0);
        wr64(&w, 0); wr64(&w, 0);                         /* btime */
        wr64(&w, 0);                                      /* gen */
        wr64(&w, 0);                                      /* data_version */
        return v9p_finish(&w);
    }

    case P9_TLOPEN: {
        uint32_t fid = rd32(&r), flags = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);

        bool wants_write = (flags & (L_O_WRONLY | L_O_RDWR | L_O_TRUNC)) != 0;
        if (wants_write && v9p_tree_ro[e->tree]) return v9p_error(&w, tag, L_EROFS);

        v9p_fid_close_handles(e);

        if (e->tree == V9P_TREE_ROOT) {
            /* Purely synthetic: nothing to open, Treaddir generates it. */
            e->is_dir = true;
        } else if (e->tree == V9P_TREE_HW) {
            if (e->hw_idx < 0) { e->is_dir = true; }
            else {
                const v9p_hw_ent *h = &v9p_hw_files[e->hw_idx];
                if (wants_write && !h->wr && h->stream != HWS_AUDIO)
                    return v9p_error(&w, tag, L_EACCES);
                if (!wants_write && !h->rd_text && h->stream != HWS_MIC &&
                    h->stream != HWS_CAM_OUT && h->stream != HWS_CAM_IN)
                    return v9p_error(&w, tag, L_EACCES);
                uint32_t rc = v9p_hw_open(e);
                if (rc) return v9p_error(&w, tag, rc);
            }
        } else {
            struct stat st;
            if (stat(e->path, &st) != 0) return v9p_error(&w, tag, L_ENOENT);
            e->is_dir = S_ISDIR(st.st_mode);
            if (e->is_dir) {
                e->dp = opendir(e->path);
                if (!e->dp) return v9p_error(&w, tag, L_EIO);
                e->dir_pos = 0;
            } else {
                const char *mode;
                if (flags & L_O_APPEND)                mode = "ab";
                else if (flags & L_O_TRUNC)            mode = "wb";
                else if (flags & (L_O_WRONLY|L_O_RDWR)) mode = "r+b";
                else                                    mode = "rb";
                e->fp = fopen(e->path, mode);
                if (!e->fp) return v9p_error(&w, tag, L_EIO);
            }
        }

        struct stat st;
        v9p_stat_path(e, &st);
        v9p_begin(&w, P9_RLOPEN, tag);
        wrqid(&w, v9p_qid_type(&st), v9p_qid_path(e->tree, e->path));
        wr32(&w, 0);      /* iounit 0 = client picks, based on msize */
        return v9p_finish(&w);
    }

    case P9_TLCREATE: {
        uint32_t fid = rd32(&r);
        char name[256];
        rdstr(&r, name, sizeof(name));
        uint32_t flags = rd32(&r), mode = rd32(&r);
        rd32(&r);                                   /* gid */
        (void)flags; (void)mode;
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[e->tree] || e->tree == V9P_TREE_HW)
            return v9p_error(&w, tag, L_EROFS);
        if (!v9p_name_ok(name)) return v9p_error(&w, tag, L_EINVAL);

        char path[V9P_PATH_MAX];
        snprintf(path, sizeof(path), "%s", e->path);
        if (!v9p_path_push(path, name)) return v9p_error(&w, tag, L_EINVAL);

        FILE *fp = fopen(path, (flags & L_O_EXCL) ? "wbx" : "wb");
        if (!fp) return v9p_error(&w, tag, L_EIO);

        /* On success the fid moves from the directory to the new file. */
        v9p_fid_close_handles(e);
        snprintf(e->path, V9P_PATH_MAX, "%s", path);
        e->fp = fp;
        e->is_dir = false;

        v9p_begin(&w, P9_RLCREATE, tag);
        wrqid(&w, P9_QTFILE, v9p_qid_path(e->tree, e->path));
        wr32(&w, 0);
        return v9p_finish(&w);
    }

    case P9_TREAD: {
        uint32_t fid = rd32(&r);
        uint64_t off = rd64(&r);
        uint32_t count = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);

        /* Clamp to whatever room the reply buffer actually has after the
           11-byte Rread header. */
        uint32_t room = (respcap > 11) ? respcap - 11 : 0;
        if (count > room) count = room;

        v9p_begin(&w, P9_RREAD, tag);
        wr32(&w, 0);                        /* count, backfilled below */
        uint32_t got = 0;

        if (e->tree == V9P_TREE_HW && e->hw_idx >= 0) {
            const v9p_hw_ent *h = &v9p_hw_files[e->hw_idx];
            if (h->stream == HWS_MIC) {
                if (w.len + count <= w.cap)
                    got = (uint32_t)hw_mic_read(w.b + w.len, (int)count);
            } else if (e->snap) {
                if (off < e->snap_len) {
                    got = e->snap_len - (uint32_t)off;
                    if (got > count) got = count;
                    if (w.len + got <= w.cap) memcpy(w.b + w.len, e->snap + off, got);
                    else got = 0;
                }
            }
        } else if (e->fp) {
            if (fseek(e->fp, (long)off, SEEK_SET) == 0 && w.len + count <= w.cap)
                got = (uint32_t)fread(w.b + w.len, 1, count, e->fp);
        } else {
            return v9p_error(&w, tag, L_EBADF);
        }

        w.len += got;
        memcpy(w.b + 7, &got, 4);
        return v9p_finish(&w);
    }

    case P9_TWRITE: {
        uint32_t fid = rd32(&r);
        uint64_t off = rd64(&r);
        uint32_t count = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        if (r.pos + count > r.len) count = r.len - r.pos;
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[e->tree]) return v9p_error(&w, tag, L_EROFS);

        int done = -1;
        if (e->tree == V9P_TREE_HW && e->hw_idx >= 0) {
            const v9p_hw_ent *h = &v9p_hw_files[e->hw_idx];
            if (h->stream == HWS_AUDIO)  done = hw_audio_write(r.b + r.pos, (int)count);
            else if (h->wr)              done = h->wr((const char *)(r.b + r.pos), (int)count);
        } else if (e->fp) {
            if (fseek(e->fp, (long)off, SEEK_SET) == 0)
                done = (int)fwrite(r.b + r.pos, 1, count, e->fp);
        }
        if (done < 0) return v9p_error(&w, tag, L_EIO);

        v9p_begin(&w, P9_RWRITE, tag);
        wr32(&w, (uint32_t)done);
        return v9p_finish(&w);
    }

    case P9_TREADDIR: {
        uint32_t fid = rd32(&r);
        uint64_t off = rd64(&r);
        uint32_t count = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (!e->is_dir) return v9p_error(&w, tag, L_ENOTDIR);

        uint32_t room = (respcap > 11) ? respcap - 11 : 0;
        if (count > room) count = room;

        v9p_begin(&w, P9_RREADDIR, tag);
        wr32(&w, 0);
        uint32_t body_start = w.len;
        uint32_t limit = w.len + count;
        if (limit > w.cap) limit = w.cap;

        /* Entry 0 is ".", entry 1 is "..", real entries follow. The cookie
           handed back is simply "index of the next entry", which is enough
           for the client's sequential reads and for the rewind-and-skip
           path below when it seeks. */
        uint64_t idx = 0;

        if (e->tree == V9P_TREE_ROOT) {
            /* ".", "..", then one directory per tree that actually came up.
               Absent trees are omitted rather than shown as broken entries,
               so `ls /mnt/3ds` is an accurate list of what's reachable. */
            for (int i = -2; i < V9P_TREE_COUNT; i++) {
                if (i >= 0 && !v9p_tree_ok[i]) continue;
                if (idx < off) { idx++; continue; }
                const char *nm = (i == -2) ? "." : (i == -1) ? ".." : v9p_tree_aname[i];
                char qpath[V9P_PATH_MAX];
                snprintf(qpath, sizeof(qpath), "/%s", (i < 0) ? "" : nm);
                uint32_t need = 13 + 8 + 1 + 2 + (uint32_t)strlen(nm);
                if (w.len + need > limit) break;
                wrqid(&w, P9_QTDIR, v9p_qid_path(i < 0 ? V9P_TREE_ROOT : i, qpath));
                wr64(&w, idx + 1);
                wr8(&w, 4);   /* DT_DIR */
                wrstr(&w, nm);
                idx++;
            }
        } else if (e->tree == V9P_TREE_HW) {
            if (e->hw_idx >= 0) return v9p_error(&w, tag, L_ENOTDIR);
            for (int i = -2; i < V9P_HW_COUNT; i++) {
                if (idx < off) { idx++; continue; }
                const char *nm; uint8_t qt; char qpath[V9P_PATH_MAX];
                if (i == -2)      { nm = ".";  qt = P9_QTDIR;  snprintf(qpath, sizeof(qpath), "/"); }
                else if (i == -1) { nm = ".."; qt = P9_QTDIR;  snprintf(qpath, sizeof(qpath), "/"); }
                else              { nm = v9p_hw_files[i].name; qt = P9_QTFILE;
                                    snprintf(qpath, sizeof(qpath), "/%s", nm); }
                uint32_t need = 13 + 8 + 1 + 2 + (uint32_t)strlen(nm);
                if (w.len + need > limit) break;
                wrqid(&w, qt, v9p_qid_path(e->tree, qpath));
                wr64(&w, idx + 1);
                wr8(&w, qt == P9_QTDIR ? 4 : 8);   /* DT_DIR / DT_REG */
                wrstr(&w, nm);
                idx++;
            }
        } else {
            if (!e->dp) return v9p_error(&w, tag, L_EBADF);
            /* A fresh scan is needed whenever the client isn't continuing
               from exactly where the last one stopped. */
            if (off == 0 || off != e->dir_pos) {
                rewinddir(e->dp);
                e->dir_pos = 0;
            }
            idx = e->dir_pos;

            while (1) {
                const char *nm;
                char qpath[V9P_PATH_MAX];
                uint8_t qt;

                if (idx == 0)      { nm = ".";  qt = P9_QTDIR; snprintf(qpath, sizeof(qpath), "%s", e->path); }
                else if (idx == 1) { nm = ".."; qt = P9_QTDIR; snprintf(qpath, sizeof(qpath), "%s", e->path);
                                     v9p_path_pop(e->tree, qpath); }
                else {
                    struct dirent *de = readdir(e->dp);
                    if (!de) break;
                    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                    nm = de->d_name;
                    snprintf(qpath, sizeof(qpath), "%s", e->path);
                    if (!v9p_path_push(qpath, nm)) continue;
                    struct stat st;
                    qt = (stat(qpath, &st) == 0 && S_ISDIR(st.st_mode)) ? P9_QTDIR : P9_QTFILE;
                }

                /* Skip forward when the client asked to resume mid-stream. */
                if (idx < off) { idx++; continue; }

                uint32_t need = 13 + 8 + 1 + 2 + (uint32_t)strlen(nm);
                if (w.len + need > limit) {
                    /* No room: this entry has already been consumed from the
                       DIR stream, so the cookie must not advance past it.
                       Force the next Treaddir to rescan by invalidating the
                       resume point. */
                    e->dir_pos = (uint64_t)-1;
                    break;
                }
                wrqid(&w, qt, v9p_qid_path(e->tree, qpath));
                wr64(&w, idx + 1);
                wr8(&w, qt == P9_QTDIR ? 4 : 8);
                wrstr(&w, nm);
                idx++;
                e->dir_pos = idx;
            }
        }

        uint32_t nbytes = w.len - body_start;
        memcpy(w.b + 7, &nbytes, 4);
        return v9p_finish(&w);
    }

    case P9_TCLUNK: {
        uint32_t fid = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (e) v9p_fid_free(e);
        v9p_begin(&w, P9_RCLUNK, tag);
        return v9p_finish(&w);
    }

    case P9_TSTATFS: {
        uint32_t fid = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        /* Report the SD card's real free space. Not cosmetic: an all-zero
           statfs makes the mount look 0 bytes big, and anything that checks
           for room before writing (installers, package managers) then
           refuses to write at all. Horizon exposes free bytes but not total
           capacity, so total is reported as the free figure - understating
           the disk is harmless where claiming zero is not. */
        uint64_t freeb = 0;
        if (e->tree == V9P_TREE_SD && v9p_sdmc_ok)
            FSUSER_GetFreeBytes(&freeb, v9p_sdmc_archive);
        /* Not every backend answers that query - Citra's virtual SD card
           reports nothing - and "0 bytes free" is the one answer that
           actively breaks callers. Fall back to a nominal figure so the
           filesystem always looks writable; it is a hint for `df`, not an
           allocation guarantee, and a real write still fails on its own if
           the card is genuinely full. */
        if (freeb == 0) freeb = 1ull << 30;   /* 1GB */
        uint64_t blocks = freeb / 512u;

        v9p_begin(&w, P9_RSTATFS, tag);
        wr32(&w, 0x01021997);   /* V9FS_MAGIC */
        wr32(&w, 512);          /* bsize */
        wr64(&w, blocks);       /* blocks */
        wr64(&w, blocks);       /* bfree */
        wr64(&w, blocks);       /* bavail */
        wr64(&w, 0); wr64(&w, 0);   /* files, ffree - no inode notion here */
        wr64(&w, 0);            /* fsid */
        wr32(&w, 255);          /* namelen */
        return v9p_finish(&w);
    }

    case P9_TMKDIR: {
        uint32_t fid = rd32(&r);
        char name[256];
        rdstr(&r, name, sizeof(name));
        rd32(&r); rd32(&r);       /* mode, gid */
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[e->tree] || e->tree == V9P_TREE_HW)
            return v9p_error(&w, tag, L_EROFS);
        if (!v9p_name_ok(name)) return v9p_error(&w, tag, L_EINVAL);
        char path[V9P_PATH_MAX];
        snprintf(path, sizeof(path), "%s", e->path);
        if (!v9p_path_push(path, name)) return v9p_error(&w, tag, L_EINVAL);
        if (mkdir(path, 0777) != 0) return v9p_error(&w, tag, L_EEXIST);
        v9p_begin(&w, P9_RMKDIR, tag);
        wrqid(&w, P9_QTDIR, v9p_qid_path(e->tree, path));
        return v9p_finish(&w);
    }

    case P9_TUNLINKAT: {
        uint32_t fid = rd32(&r);
        char name[256];
        rdstr(&r, name, sizeof(name));
        rd32(&r);                 /* flags: AT_REMOVEDIR - rmdir/remove both work */
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[e->tree] || e->tree == V9P_TREE_HW)
            return v9p_error(&w, tag, L_EROFS);
        if (!v9p_name_ok(name)) return v9p_error(&w, tag, L_EINVAL);
        char path[V9P_PATH_MAX];
        snprintf(path, sizeof(path), "%s", e->path);
        if (!v9p_path_push(path, name)) return v9p_error(&w, tag, L_EINVAL);
        struct stat st;
        if (stat(path, &st) != 0) return v9p_error(&w, tag, L_ENOENT);
        int rc = S_ISDIR(st.st_mode) ? rmdir(path) : remove(path);
        if (rc != 0) return v9p_error(&w, tag, S_ISDIR(st.st_mode) ? L_ENOTEMPTY : L_EIO);
        v9p_begin(&w, P9_RUNLINKAT, tag);
        return v9p_finish(&w);
    }

    case P9_TRENAMEAT: {
        uint32_t ofid = rd32(&r);
        char oname[256];
        rdstr(&r, oname, sizeof(oname));
        uint32_t nfid = rd32(&r);
        char nname[256];
        rdstr(&r, nname, sizeof(nname));
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *oe = v9p_fid_find(ofid), *ne = v9p_fid_find(nfid);
        if (!oe || !ne) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[oe->tree] || v9p_tree_ro[ne->tree] ||
            oe->tree == V9P_TREE_HW || ne->tree == V9P_TREE_HW)
            return v9p_error(&w, tag, L_EROFS);
        if (!v9p_name_ok(oname) || !v9p_name_ok(nname))
            return v9p_error(&w, tag, L_EINVAL);
        char op[V9P_PATH_MAX], np[V9P_PATH_MAX];
        snprintf(op, sizeof(op), "%s", oe->path);
        snprintf(np, sizeof(np), "%s", ne->path);
        if (!v9p_path_push(op, oname) || !v9p_path_push(np, nname))
            return v9p_error(&w, tag, L_EINVAL);
        if (rename(op, np) != 0) return v9p_error(&w, tag, L_EIO);
        v9p_begin(&w, P9_RRENAMEAT, tag);
        return v9p_finish(&w);
    }

    case P9_TSETATTR: {
        uint32_t fid = rd32(&r);
        uint32_t valid = rd32(&r);
        rd32(&r); rd32(&r); rd32(&r);      /* mode, uid, gid */
        uint64_t size = rd64(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[e->tree]) return v9p_error(&w, tag, L_EROFS);
        /* Only truncation is actually actionable: FAT has no ownership and
           no usable permission bits, so chmod/chown are accepted and
           ignored rather than failed, which would break ordinary tools like
           `cp -p` for no benefit. */
        if ((valid & 0x8 /* ATTR_SIZE */) && e->tree != V9P_TREE_HW) {
            if (truncate(e->path, (off_t)size) != 0)
                return v9p_error(&w, tag, L_EIO);
        }
        v9p_begin(&w, P9_RSETATTR, tag);
        return v9p_finish(&w);
    }

    case P9_TFSYNC: {
        uint32_t fid = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (e && e->fp) fflush(e->fp);
        v9p_begin(&w, P9_RFSYNC, tag);
        return v9p_finish(&w);
    }

    case P9_TFLUSH:
        /* Requests are serviced synchronously, so by the time a Tflush is
           parsed the request it refers to has already completed. */
        v9p_begin(&w, P9_RFLUSH, tag);
        return v9p_finish(&w);

    case P9_TLOCK:
        /* Granting locks unconditionally is safe for a single-guest
           machine and stops anything using flock() from failing outright. */
        rd32(&r);
        v9p_begin(&w, P9_RLOCK, tag);
        wr8(&w, 0);                 /* P9_LOCK_SUCCESS */
        return v9p_finish(&w);

    case P9_TGETLOCK: {
        rd32(&r);
        uint8_t ltype = rd8(&r);
        v9p_begin(&w, P9_RGETLOCK, tag);
        wr8(&w, ltype);
        wr64(&w, 0); wr64(&w, 0);
        wr32(&w, 0);
        wrstr(&w, "");
        return v9p_finish(&w);
    }

    case P9_TREMOVE: {
        uint32_t fid = rd32(&r);
        if (r.err) return v9p_error(&w, tag, L_EINVAL);
        v9p_fid *e = v9p_fid_find(fid);
        if (!e) return v9p_error(&w, tag, L_EBADF);
        if (v9p_tree_ro[e->tree] || e->tree == V9P_TREE_HW)
            return v9p_error(&w, tag, L_EROFS);
        char path[V9P_PATH_MAX];
        snprintf(path, sizeof(path), "%s", e->path);
        bool isdir = e->is_dir;
        v9p_fid_free(e);
        if ((isdir ? rmdir(path) : remove(path)) != 0)
            return v9p_error(&w, tag, L_EIO);
        v9p_begin(&w, P9_RREMOVE, tag);
        return v9p_finish(&w);
    }

    /* Unsupported by design. FAT has no symlinks, device nodes or extended
       attributes, and NAND is read-only, so these can only ever fail —
       answering promptly is better than pretending. EOPNOTSUPP on
       Txattrwalk in particular is the documented way to tell the client
       "this filesystem has no xattrs", which stops it retrying. */
    case P9_TXATTRWALK:
    case P9_TXATTRCREATE:
        return v9p_error(&w, tag, L_EOPNOTSUPP);
    case P9_TSYMLINK:
    case P9_TMKNOD:
    case P9_TLINK:
    case P9_TREADLINK:
        return v9p_error(&w, tag, L_ENOSYS);

    default:
        return v9p_error(&w, tag, L_ENOSYS);
    }
}

/* ------------------------------------------------------------------
 * Virtqueue plumbing
 * ------------------------------------------------------------------ */

static uint8_t v9p_req_buf[V9P_MSIZE];
static uint8_t v9p_resp_buf[V9P_MSIZE];

static void v9p_process_queue(uint8_t *ram) {
    if (!v9p.queue_ready || !v9p.queue_desc_lo) return;

    uint8_t *desc_table = guest_ptr(ram, v9p.queue_desc_lo, VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, v9p.queue_driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, v9p.queue_device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    uint16_t avail_idx;
    memcpy(&avail_idx, avail_ring + 2, 2);

    while (v9p.last_avail_idx != avail_idx) {
        uint16_t ring_pos = v9p.last_avail_idx % VQUEUE_SIZE;
        uint16_t head;
        memcpy(&head, avail_ring + 4u + ring_pos * 2u, 2);
        v9p.last_avail_idx++;

        /* Gather the readable half of the chain into one flat request, and
           remember the writable half so the reply can be scattered back.
           The 9p transport splits large reads/writes across several
           descriptors (its zero-copy path hands over user pages directly),
           so neither half is a single buffer in general. */
        uint32_t wlen[64];
        uint64_t waddr[64];
        int      nw = 0;
        uint32_t reqlen = 0;

        uint16_t di = head;
        for (int guard = 0; guard < 64; guard++) {
            uint64_t a; uint32_t l; uint16_t f, nx;
            vblk_read_desc(desc_table, di, &a, &l, &f, &nx);

            if (f & VDESC_F_WRITE) {
                if (nw < 64) { waddr[nw] = a; wlen[nw] = l; nw++; }
            } else {
                uint8_t *p = guest_ptr(ram, (uint32_t)a, l);
                if (p && reqlen + l <= sizeof(v9p_req_buf)) {
                    memcpy(v9p_req_buf + reqlen, p, l);
                    reqlen += l;
                }
            }
            if (!(f & VDESC_F_NEXT)) break;
            di = nx;
        }

        uint32_t wcap = 0;
        for (int i = 0; i < nw; i++) wcap += wlen[i];
        if (wcap > sizeof(v9p_resp_buf)) wcap = sizeof(v9p_resp_buf);

        uint32_t rlen = 0;
        if (reqlen >= 7 && wcap >= 7)
            rlen = v9p_handle(v9p_req_buf, reqlen, v9p_resp_buf, wcap);

        /* Scatter the reply across the writable descriptors in order. */
        uint32_t left = rlen, off = 0;
        for (int i = 0; i < nw && left; i++) {
            uint32_t n = wlen[i] < left ? wlen[i] : left;
            uint8_t *p = guest_ptr(ram, (uint32_t)waddr[i], n);
            if (!p) break;
            memcpy(p, v9p_resp_buf + off, n);
            off += n; left -= n;
        }

        uint16_t used_idx_val;
        memcpy(&used_idx_val, used_ring + 2, 2);
        uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
        uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
        uint32_t hd32 = head;
        memcpy(ue + 0, &hd32, 4);
        memcpy(ue + 4, &rlen, 4);
        used_idx_val++;
        memcpy(used_ring + 2, &used_idx_val, 2);

        v9p.int_status |= 1u;
    }

    if (v9p.int_status) plic_set_pending(PLIC_SRC_9P, true);
}

/* ------------------------------------------------------------------
 * Init + MMIO
 * ------------------------------------------------------------------ */

static void v9p_init(void) {
    memset(&v9p, 0, sizeof(v9p));
    v9p.queue_num = VQUEUE_SIZE;
    v9p.msize     = V9P_MSIZE;

    hw3ds_init();

    v9p_tree_ok[V9P_TREE_ROOT] = true; /* the synthetic listing always exists */
    v9p_tree_ok[V9P_TREE_SD] = true;   /* sdmc is always mounted for homebrew */
    v9p_tree_ok[V9P_TREE_HW] = true;

    /* NAND needs permissions plain homebrew doesn't get. Under Luma3DS with
       extended homebrew perms these succeed; otherwise the trees simply
       stay absent and an attach to them fails cleanly. */
    FS_Path empty = { PATH_EMPTY, 1, "" };
    v9p_sdmc_ok = R_SUCCEEDED(FSUSER_OpenArchive(&v9p_sdmc_archive, ARCHIVE_SDMC, empty));
    v9p_tree_ok[V9P_TREE_NAND] =
        R_SUCCEEDED(archiveMount(ARCHIVE_NAND_CTR_FS, empty, "v9nand"));
    v9p_tree_ok[V9P_TREE_TWL] =
        R_SUCCEEDED(archiveMount(ARCHIVE_NAND_TWL_FS, empty, "v9twl"));
}

static void v9p_exit(void) {
    for (int i = 0; i < V9P_MAX_FIDS; i++)
        if (v9p.fids[i].used) v9p_fid_free(&v9p.fids[i]);
    if (v9p_tree_ok[V9P_TREE_NAND]) archiveUnmount("v9nand");
    if (v9p_tree_ok[V9P_TREE_TWL])  archiveUnmount("v9twl");
    if (v9p_sdmc_ok) { FSUSER_CloseArchive(v9p_sdmc_archive); v9p_sdmc_ok = false; }
    hw3ds_exit();
}

/* Config space: tag_len[2] then the tag bytes. The driver reads the length
   as a halfword and the tag a byte at a time, and MMIO accesses arrive here
   byte-addressed regardless of width, so both work out.

   noinline is load-bearing, not a style choice. Inlined into v9p_load, the
   index into the tag is `addy - 0x10006102`, and GCC strength-reduces
   `V9P_MOUNT_TAG[o - 2]` into `*((&tag - 0x10006102) + addy)`, parking that
   folded base in a literal pool. `&tag - 0x10006102` wraps to 0xF0xxxxxx,
   and a 3DSX absolute relocation stores its target in the low 28 bits with
   the top nibble reserved as a sub-type tag — so the value is unencodable,
   and every conformant 3DSX loader (Citra and the real hbmenu one both)
   rejects the whole file with a relocation error. Behind a noinline call
   the offset arrives as a plain parameter based at 0, the fold can't span
   the MMIO base, and the literal is just `&tag`.

   Any new device that indexes a *global* by an offset derived from the raw
   MMIO address is one optimiser decision away from the same trap. Keep that
   arithmetic on locals, or behind a boundary like this one. */
static __attribute__((noinline)) uint32_t v9p_config_read(uint32_t o) {
    uint32_t taglen = (uint32_t)strlen(V9P_MOUNT_TAG);
    if (o == 0) return taglen;
    if (o >= 2 && o < 2 + taglen) return (uint8_t)V9P_MOUNT_TAG[o - 2];
    return 0;
}

static uint32_t v9p_load(uint32_t addy) {
    uint32_t r = addy - VIRTIO_9P_BASE;

    if (r >= 0x100) return v9p_config_read(r - 0x100);

    switch (r) {
    case VREG_MAGIC:           return 0x74726976u;
    case VREG_VERSION:         return 2u;
    case VREG_DEVICE_ID:       return 9u;   /* 9P transport */
    case VREG_VENDOR_ID:       return 0x554d4551u;
    case VREG_DEVICE_FEATURES:
        return (v9p.dev_feat_sel == 1) ? VIRTIO_F_VERSION_1_HI
                                       : VIRTIO_9P_F_MOUNT_TAG;
    case VREG_QUEUE_NUM_MAX:   return VQUEUE_SIZE;
    case VREG_QUEUE_READY:     return v9p.queue_ready;
    case VREG_INT_STATUS:      return v9p.int_status;
    case VREG_STATUS:          return v9p.status;
    case VREG_CONFIG_GEN:      return 0u;
    default:                   return 0u;
    }
}

static void v9p_store(uint32_t addy, uint32_t val, uint8_t *ram) {
    uint32_t r = addy - VIRTIO_9P_BASE;
    switch (r) {
    case VREG_DEV_FEAT_SEL:    v9p.dev_feat_sel = val; break;
    case VREG_DRV_FEAT_SEL:    v9p.drv_feat_sel = val; break;
    case VREG_DRIVER_FEATURES: break;
    case VREG_QUEUE_SEL:       break;   /* single queue */
    case VREG_QUEUE_NUM:       v9p.queue_num = val < VQUEUE_SIZE ? val : VQUEUE_SIZE; break;
    case VREG_QUEUE_READY:     v9p.queue_ready = val; break;
    case VREG_QUEUE_NOTIFY:    v9p_process_queue(ram); break;
    case VREG_INT_ACK:
        v9p.int_status &= ~val;
        plic_set_pending(PLIC_SRC_9P, v9p.int_status != 0);
        break;
    case VREG_STATUS:
        v9p.status = val;
        if (val == 0) {
            v9p.queue_ready = 0;
            v9p.int_status = 0;
            v9p.last_avail_idx = 0;
            for (int i = 0; i < V9P_MAX_FIDS; i++)
                if (v9p.fids[i].used) v9p_fid_free(&v9p.fids[i]);
            plic_set_pending(PLIC_SRC_9P, false);
        }
        break;
    case VREG_QUEUE_DESC_LO:   v9p.queue_desc_lo   = val; break;
    case VREG_QUEUE_DRIVER_LO: v9p.queue_driver_lo = val; break;
    case VREG_QUEUE_DEVICE_LO: v9p.queue_device_lo = val; break;
    default: break;
    }
}
