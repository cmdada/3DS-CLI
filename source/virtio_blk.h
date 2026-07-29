#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Virtio-mmio block device emulation.
 *
 * Implements the virtio-mmio 2.0 transport + virtio-blk device.
 * The device lives at VIRTIO_BLK_BASE (0x10001000) within the existing
 * mini-rv32ima MMIO window, so MINIRV32_MMIO_RANGE needs no changes.
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
static struct {
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
} vblk;

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
static void vblk_read_desc(uint8_t *desc_table, uint16_t idx,
                            uint64_t *addr, uint32_t *len,
                            uint16_t *flags, uint16_t *next) {
    uint8_t *d = desc_table + (uint32_t)idx * 16u;
    memcpy(addr,  d + 0, 8);
    memcpy(len,   d + 8, 4);
    memcpy(flags, d + 12, 2);
    memcpy(next,  d + 14, 2);
}

/* Process all pending virtq entries when the guest rings the doorbell */
static void vblk_process_queue(uint8_t *ram) {
    if (!vblk.queue_ready || !vblk.disk || !vblk.queue_desc_lo) return;

    uint8_t *desc_table = guest_ptr(ram, vblk.queue_desc_lo,
                                    VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, vblk.queue_driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, vblk.queue_device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    /* avail ring layout: flags(2) idx(2) ring[N](2 each) */
    uint16_t avail_idx;
    memcpy(&avail_idx, avail_ring + 2, 2);

    while (vblk.last_avail_idx != avail_idx) {
        uint16_t ring_pos = vblk.last_avail_idx % VQUEUE_SIZE;
        uint16_t head;
        memcpy(&head, avail_ring + 4u + ring_pos * 2u, 2);
        vblk.last_avail_idx++;

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
            memcpy(&req_type, hdr + 0, 4);
            memcpy(&sector,   hdr + 8, 8);

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
                        fseek(vblk.disk, disk_off, SEEK_SET);
                        size_t n = fread(buf, 1, dl, vblk.disk);
                        bytes_xfer += (uint32_t)n;
                        sector     += dl / SECTOR_SZ;
                    } else if (req_type == VBLK_T_OUT) {
                        fseek(vblk.disk, disk_off, SEEK_SET);
                        fwrite(buf, 1, dl, vblk.disk);
                        bytes_xfer += dl;
                        sector     += dl / SECTOR_SZ;
                    } else if (req_type == VBLK_T_FLUSH) {
                        fflush(vblk.disk);
                    }
                }
            }

            /* Write completion to used ring */
            /* used layout: flags(2) idx(2) ring[N]: id(4) len(4) */
            uint16_t used_idx_val;
            memcpy(&used_idx_val, used_ring + 2, 2);
            uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
            uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
            uint32_t hd32 = head;
            memcpy(ue + 0, &hd32,       4);
            memcpy(ue + 4, &bytes_xfer, 4);
            used_idx_val++;
            memcpy(used_ring + 2, &used_idx_val, 2);
        }

        vblk.int_status |= 1u; /* VIRTIO_INT_VRING */
    }

    /* Notify the PLIC if any completions produced */
    if (vblk.int_status) plic_set_pending(PLIC_SRC_BLK, true);
}

/* ------------------------------------------------------------------
 * MMIO interface (called from HandleControlLoad / HandleControlStore)
 * ------------------------------------------------------------------ */
static void vblk_init(FILE *disk, uint64_t size_bytes) {
    memset(&vblk, 0, sizeof(vblk));
    vblk.disk              = disk;
    vblk.capacity_sectors  = size_bytes / SECTOR_SZ;
    vblk.queue_num         = VQUEUE_SIZE;
}

static uint32_t vblk_load(uint32_t addy) {
    uint32_t r = addy - VIRTIO_BLK_BASE;
    switch (r) {
    case VREG_MAGIC:           return 0x74726976u; /* "virt" */
    case VREG_VERSION:         return 2u;
    case VREG_DEVICE_ID:       return 2u;          /* block device */
    case VREG_VENDOR_ID:       return 0x554d4551u; /* "QEMU" */
    case VREG_DEVICE_FEATURES: return (vblk.dev_feat_sel == 1) ? VIRTIO_F_VERSION_1_HI : 0u;
    case VREG_QUEUE_NUM_MAX:   return VQUEUE_SIZE;
    case VREG_QUEUE_READY:     return vblk.queue_ready;
    case VREG_INT_STATUS:      return vblk.int_status;
    case VREG_STATUS:          return vblk.status;
    case VREG_CONFIG_GEN:      return 0u;
    case VREG_BLK_CAP_LO:     return (uint32_t)(vblk.capacity_sectors & 0xffffffffu);
    case VREG_BLK_CAP_HI:     return (uint32_t)(vblk.capacity_sectors >> 32);
    default:                   return 0u;
    }
}

static void vblk_store(uint32_t addy, uint32_t val, uint8_t *ram) {
    uint32_t r = addy - VIRTIO_BLK_BASE;
    switch (r) {
    case VREG_DEV_FEAT_SEL:   vblk.dev_feat_sel    = val; break;
    case VREG_DRV_FEAT_SEL:   vblk.drv_feat_sel    = val; break;
    case VREG_DRIVER_FEATURES:
        if (vblk.drv_feat_sel == 0) vblk.drv_features_lo = val;
        else                        vblk.drv_features_hi = val;
        break;
    case VREG_QUEUE_SEL:       /* only queue 0 */ break;
    case VREG_QUEUE_NUM:       vblk.queue_num       = val < VQUEUE_SIZE ? val : VQUEUE_SIZE; break;
    case VREG_QUEUE_READY:     vblk.queue_ready     = val; break;
    case VREG_QUEUE_NOTIFY:    vblk_process_queue(ram); break;
    case VREG_INT_ACK:
        vblk.int_status &= ~val;
        plic_set_pending(PLIC_SRC_BLK, vblk.int_status != 0);
        break;
    case VREG_STATUS:
        vblk.status = val;
        if (val == 0) {
            /* driver-triggered reset */
            vblk.queue_ready = 0;
            vblk.int_status  = 0;
            vblk.last_avail_idx = 0;
            plic_set_pending(PLIC_SRC_BLK, false);
        }
        break;
    case VREG_QUEUE_DESC_LO:   vblk.queue_desc_lo   = val; break;
    case VREG_QUEUE_DESC_HI:   break; /* 32-bit only */
    case VREG_QUEUE_DRIVER_LO: vblk.queue_driver_lo = val; break;
    case VREG_QUEUE_DRIVER_HI: break;
    case VREG_QUEUE_DEVICE_LO: vblk.queue_device_lo = val; break;
    case VREG_QUEUE_DEVICE_HI: break;
    default: break;
    }
}
