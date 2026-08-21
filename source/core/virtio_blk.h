#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Virtio-mmio block device emulation.
 *
 * Implements the virtio-mmio 2.0 transport + virtio-blk device.
 * Two instances exist, both within the existing mini-rv32ima MMIO window
 * (so MINIRV32_MMIO_RANGE needs no changes): the rootfs at
 * VIRTIO_BLK_BASE (0x10001000, /dev/vda) and the swap device at
 * VIRTIO_BLK2_BASE (0x10005000, /dev/vdb). They share all of the code
 * below; everything device-specific lives in vblk_dev_t.
 *
 * Interrupts: completion sets MEIP (mip bit 11) directly on the emulated
 * CPU.  The DTB wires the virtio-mmio interrupt-parent to riscv,cpu-intc
 * with interrupt number 11, so no PLIC is needed.
 *
 * All guest physical addresses are relative to RISCV_RAM_BASE (0x80000000).
 * On NOMMU Linux, virt == phys, so we just subtract the base.
 */

#define VIRTIO_BLK_BASE   0x10001000u
#define VIRTIO_BLK_SIZE   0x1000u
#define VIRTIO_BLK2_BASE  0x10005000u   /* swap device (see main.c) */
#define VBLK_COUNT        2u
#define RISCV_RAM_BASE    0x80000000u
#define SECTOR_SZ         512u
/* Must be a power of 2. Must also be >= 2+MAX_SKB_FRAGS+... (~19) for
   virtio-net: its xmit path stops the netif queue whenever fewer than that
   many descriptors are free, so a smaller ring permanently stalls TX after
   the very first packet and the guest can never even complete DHCP. */
#define VQUEUE_SIZE       128u

/* virtio-mmio register offsets */
#define VREG_MAGIC            0x000
#define VREG_VERSION          0x004
#define VREG_DEVICE_ID        0x008
#define VREG_VENDOR_ID        0x00c
#define VREG_DEVICE_FEATURES  0x010
#define VREG_DEV_FEAT_SEL     0x014
#define VREG_DRIVER_FEATURES  0x020
#define VREG_DRV_FEAT_SEL     0x024
#define VREG_QUEUE_SEL        0x030
#define VREG_QUEUE_NUM_MAX    0x034
#define VREG_QUEUE_NUM        0x038
#define VREG_QUEUE_READY      0x044
#define VREG_QUEUE_NOTIFY     0x050
#define VREG_INT_STATUS       0x060
#define VREG_INT_ACK          0x064
#define VREG_STATUS           0x070
#define VREG_QUEUE_DESC_LO    0x080
#define VREG_QUEUE_DESC_HI    0x084
#define VREG_QUEUE_DRIVER_LO  0x090
#define VREG_QUEUE_DRIVER_HI  0x094
#define VREG_QUEUE_DEVICE_LO  0x0a0
#define VREG_QUEUE_DEVICE_HI  0x0a4
#define VREG_CONFIG_GEN       0x0fc
#define VREG_BLK_CAP_LO      0x100   /* device-specific: capacity in sectors */
#define VREG_BLK_CAP_HI      0x104

/* virtio-blk request types */
#define VBLK_T_IN    0u   /* disk → guest */
#define VBLK_T_OUT   1u   /* guest → disk */
#define VBLK_T_FLUSH 4u

/* virtq descriptor flags */
#define VDESC_F_NEXT  1u
#define VDESC_F_WRITE 2u

/* virtio status field bits (in the 1-byte status field at end of chain) */
#define VBLK_S_OK    0u
#define VBLK_S_IOERR 1u

/* virtio feature bit: VIRTIO_F_VERSION_1 is bit 32 (sel=1, bit 0) */
#define VIRTIO_F_VERSION_1_HI 0x00000001u

/* MIP_MEIP is defined in plic.h (included before this file) */

/* ------------------------------------------------------------------
 * Device state
 * ------------------------------------------------------------------ */
typedef struct {
    uint32_t dev_feat_sel;
    uint32_t drv_feat_sel;
    uint32_t drv_features_lo;
    uint32_t drv_features_hi;
    uint32_t queue_num;          /* driver-configured queue size */
    uint32_t queue_ready;
    uint32_t int_status;
    uint32_t status;
    uint32_t queue_desc_lo;
    uint32_t queue_driver_lo;    /* avail ring */
    uint32_t queue_device_lo;    /* used ring */
    uint16_t last_avail_idx;
    FILE    *disk;
    uint64_t capacity_sectors;
    uint32_t base;               /* MMIO window this instance answers on */
    uint32_t plic_src;           /* PLIC source it raises completions on */
} vblk_dev_t;

static vblk_dev_t vblk_devs[VBLK_COUNT];

/* MiniRV32IMAState *core must be visible where virtio_blk.h is included */
extern struct MiniRV32IMAState *core;

static inline uint8_t *guest_ptr(uint8_t *ram, uint32_t gpa, uint32_t len) {
    if (gpa < RISCV_RAM_BASE) return NULL;
    uint32_t ofs = gpa - RISCV_RAM_BASE;
    /* Reject anything that doesn't fit entirely within RAM, including
       overflow of ofs+len itself - a bad/garbage guest-supplied address
       must never turn into an out-of-bounds host pointer. This range
       covers the MiniRV32IMAState struct living at the tail of ram_image,
       so an unchecked access here can silently corrupt live CPU state
       (this is exactly what was happening: mie/mip/etc getting clobbered
       by virtio_net/virtio_rng descriptors with unvalidated addresses). */
    if (ofs >= MINI_RV32_RAM_SIZE) return NULL;
    if (len > MINI_RV32_RAM_SIZE - ofs) return NULL;
    return ram + ofs;
}

/* Read virtq descriptor fields via memcpy (avoids unaligned-access UB) */
/* The destination of a disk read is wherever in guest RAM the driver pointed
   its descriptor, which is aligned to nothing in particular. Some consoles'
   filesystems require an aligned destination - the Wii U's wants 0x40 - so
   there the transfer goes through an aligned buffer and is copied across.
   Consoles without the constraint read straight into guest memory, which is
   what this always did. */
#ifdef PLAT_FS_NEEDS_ALIGNED_IO
#define VBLK_BOUNCE_SZ (32u * 1024u)
static uint8_t vblk_bounce[VBLK_BOUNCE_SZ] __attribute__((aligned(64)));

static size_t vblk_read(FILE *f, uint8_t *dst, uint32_t len) {
    size_t total = 0;
    while (len) {
        uint32_t n = len < VBLK_BOUNCE_SZ ? len : VBLK_BOUNCE_SZ;
        size_t got = fread(vblk_bounce, 1, n, f);
        if (!got) break;
        memcpy(dst, vblk_bounce, got);
        dst += got; total += got; len -= (uint32_t)got;
        if (got < n) break;
    }
    return total;
}

static void vblk_write(FILE *f, const uint8_t *src, uint32_t len) {
    while (len) {
        uint32_t n = len < VBLK_BOUNCE_SZ ? len : VBLK_BOUNCE_SZ;
        memcpy(vblk_bounce, src, n);
        if (fwrite(vblk_bounce, 1, n, f) != n) break;
        src += n; len -= n;
    }
}
#else
static inline size_t vblk_read(FILE *f, uint8_t *dst, uint32_t len) {
    return fread(dst, 1, len, f);
}
static inline void vblk_write(FILE *f, const uint8_t *src, uint32_t len) {
    fwrite(src, 1, len, f);
}
#endif

static void vblk_read_desc(uint8_t *desc_table, uint16_t idx,
                            uint64_t *addr, uint32_t *len,
                            uint16_t *flags, uint16_t *next) {
    uint8_t *d = desc_table + (uint32_t)idx * 16u;
    *addr  = g_ld64(d + 0);
    *len   = g_ld32(d + 8);
    *flags = g_ld16(d + 12);
    *next  = g_ld16(d + 14);
}

/* Process all pending virtq entries when the guest rings the doorbell */
static void vblk_process_queue(vblk_dev_t *d, uint8_t *ram) {
    if (!d->queue_ready || !d->disk || !d->queue_desc_lo) return;

    uint8_t *desc_table = guest_ptr(ram, d->queue_desc_lo,
                                    VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, d->queue_driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, d->queue_device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    /* avail ring layout: flags(2) idx(2) ring[N](2 each) */
    uint16_t avail_idx;
    avail_idx = g_ld16(avail_ring + 2);

    while (d->last_avail_idx != avail_idx) {
        uint16_t ring_pos = d->last_avail_idx % VQUEUE_SIZE;
        uint16_t head;
        head = g_ld16(avail_ring + 4u + ring_pos * 2u);
        d->last_avail_idx++;

        /* Collect descriptor chain (max 64 entries) */
        uint16_t chain[64];
        int chain_len = 0;
        uint16_t di = head;
        for (;;) {
            if (chain_len >= 64) break;
            chain[chain_len++] = di;
            uint64_t a; uint32_t l; uint16_t f, nx;
            vblk_read_desc(desc_table, di, &a, &l, &f, &nx);
            if (!(f & VDESC_F_NEXT)) break;
            di = nx;
        }
        if (chain_len < 2) continue; /* malformed: need at least header + status */

        /* First descriptor = virtio_blk_req header (type[4], reserved[4], sector[8]) */
        {
            uint64_t a; uint32_t l; uint16_t f, nx;
            vblk_read_desc(desc_table, chain[0], &a, &l, &f, &nx);
            uint8_t *hdr = guest_ptr(ram, (uint32_t)a, 16);
            if (!hdr) continue;

            uint32_t req_type;
            uint64_t sector;
            req_type = g_ld32(hdr + 0);
            sector = g_ld64(hdr + 8);

            uint8_t  req_status   = VBLK_S_OK;
            uint32_t bytes_xfer   = 0;

            /* Middle descriptors = data buffers; last descriptor = status byte */
            for (int i = 1; i < chain_len; i++) {
                uint64_t da; uint32_t dl; uint16_t df, dnx;
                vblk_read_desc(desc_table, chain[i], &da, &dl, &df, &dnx);
                uint8_t *buf = guest_ptr(ram, (uint32_t)da, dl);

                if (i == chain_len - 1) {
                    /* Status byte */
                    if (buf) buf[0] = req_status;
                } else if (buf) {
                    /* Data transfer */
                    long disk_off = (long)(sector * SECTOR_SZ);
                    if (req_type == VBLK_T_IN) {
                        fseek(d->disk, disk_off, SEEK_SET);
                        bytes_xfer += (uint32_t)vblk_read(d->disk, buf, dl);
                        sector     += dl / SECTOR_SZ;
                    } else if (req_type == VBLK_T_OUT) {
                        fseek(d->disk, disk_off, SEEK_SET);
                        vblk_write(d->disk, buf, dl);
                        bytes_xfer += dl;
                        sector     += dl / SECTOR_SZ;
                    } else if (req_type == VBLK_T_FLUSH) {
                        fflush(d->disk);
                    }
                }
            }

            /* Write completion to used ring */
            /* used layout: flags(2) idx(2) ring[N]: id(4) len(4) */
            uint16_t used_idx_val;
            used_idx_val = g_ld16(used_ring + 2);
            uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
            uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
            uint32_t hd32 = head;
            g_st32(ue + 0, hd32);
            g_st32(ue + 4, bytes_xfer);
            used_idx_val++;
            g_st16(used_ring + 2, used_idx_val);
        }

        d->int_status |= 1u; /* VIRTIO_INT_VRING */
    }

    /* Notify the PLIC if any completions produced */
    if (d->int_status) plic_set_pending(d->plic_src, true);
}

/* ------------------------------------------------------------------
 * MMIO interface (called from HandleControlLoad / HandleControlStore)
 * ------------------------------------------------------------------ */
static void vblk_init(unsigned idx, FILE *disk, uint64_t size_bytes,
                      uint32_t base, uint32_t plic_src) {
    vblk_dev_t *d = &vblk_devs[idx];
    memset(d, 0, sizeof(*d));
    d->disk              = disk;
    d->capacity_sectors  = size_bytes / SECTOR_SZ;
    d->queue_num         = VQUEUE_SIZE;
    d->base              = base;
    d->plic_src          = plic_src;
}

/* Which instance (if any) owns this MMIO address. An instance whose backing
   file was never opened (base == 0, e.g. swap when the file couldn't be
   created) matches nothing, so the guest simply sees no device there. */
static vblk_dev_t *vblk_for_addr(uint32_t addy) {
    for (unsigned i = 0; i < VBLK_COUNT; i++) {
        vblk_dev_t *d = &vblk_devs[i];
        if (d->base && addy >= d->base && addy < d->base + VIRTIO_BLK_SIZE) return d;
    }
    return NULL;
}

static uint32_t vblk_load(vblk_dev_t *d, uint32_t addy) {
    uint32_t r = addy - d->base;
    switch (r) {
    case VREG_MAGIC:           return 0x74726976u; /* "virt" */
    case VREG_VERSION:         return 2u;
    case VREG_DEVICE_ID:       return 2u;          /* block device */
    case VREG_VENDOR_ID:       return 0x554d4551u; /* "QEMU" */
    case VREG_DEVICE_FEATURES: return (d->dev_feat_sel == 1) ? VIRTIO_F_VERSION_1_HI : 0u;
    case VREG_QUEUE_NUM_MAX:   return VQUEUE_SIZE;
    case VREG_QUEUE_READY:     return d->queue_ready;
    case VREG_INT_STATUS:      return d->int_status;
    case VREG_STATUS:          return d->status;
    case VREG_CONFIG_GEN:      return 0u;
    case VREG_BLK_CAP_LO:     return (uint32_t)(d->capacity_sectors & 0xffffffffu);
    case VREG_BLK_CAP_HI:     return (uint32_t)(d->capacity_sectors >> 32);
    default:                   return 0u;
    }
}

static void vblk_store(vblk_dev_t *d, uint32_t addy, uint32_t val, uint8_t *ram) {
    uint32_t r = addy - d->base;
    switch (r) {
    case VREG_DEV_FEAT_SEL:   d->dev_feat_sel    = val; break;
    case VREG_DRV_FEAT_SEL:   d->drv_feat_sel    = val; break;
    case VREG_DRIVER_FEATURES:
        if (d->drv_feat_sel == 0) d->drv_features_lo = val;
        else                      d->drv_features_hi = val;
        break;
    case VREG_QUEUE_SEL:       /* only queue 0 */ break;
    case VREG_QUEUE_NUM:       d->queue_num       = val < VQUEUE_SIZE ? val : VQUEUE_SIZE; break;
    case VREG_QUEUE_READY:     d->queue_ready     = val; break;
    case VREG_QUEUE_NOTIFY:    vblk_process_queue(d, ram); break;
    case VREG_INT_ACK:
        d->int_status &= ~val;
        plic_set_pending(d->plic_src, d->int_status != 0);
        break;
    case VREG_STATUS:
        d->status = val;
        if (val == 0) {
            /* driver-triggered reset */
            d->queue_ready = 0;
            d->int_status  = 0;
            d->last_avail_idx = 0;
            plic_set_pending(d->plic_src, false);
        }
        break;
    case VREG_QUEUE_DESC_LO:   d->queue_desc_lo   = val; break;
    case VREG_QUEUE_DESC_HI:   break; /* 32-bit only */
    case VREG_QUEUE_DRIVER_LO: d->queue_driver_lo = val; break;
    case VREG_QUEUE_DRIVER_HI: break;
    case VREG_QUEUE_DEVICE_LO: d->queue_device_lo = val; break;
    case VREG_QUEUE_DEVICE_HI: break;
    default: break;
    }
}
