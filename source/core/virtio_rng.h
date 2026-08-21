#pragma once
#include <stdint.h>
#include <string.h>
#include "plat.h"

/*
 * Virtio-mmio entropy (RNG) device.
 *
 * Wires the RISC-V guest's virtio_rng driver to the 3DS's real hardware
 * random number generator where the console has one (plat_random),
 * so /dev/hwrng and early boot entropy come from real hardware instead of
 * the kernel's own (unseeded, NOMMU-limited) entropy pool.
 *
 * Single virtqueue: guest posts write-only buffers, device fills them with
 * random bytes and completes them immediately (no real async I/O needed).
 */

#define VIRTIO_RNG_BASE   0x10003000u
#define VIRTIO_RNG_SIZE   0x1000u

static struct {
    uint32_t dev_feat_sel;
    uint32_t drv_feat_sel;
    uint32_t queue_num;
    uint32_t queue_ready;
    uint32_t int_status;
    uint32_t status;
    uint32_t queue_desc_lo;
    uint32_t queue_driver_lo;
    uint32_t queue_device_lo;
    uint16_t last_avail_idx;
    bool     ps_ready;
} vrng;

static void vrng_init(void) {
    memset(&vrng, 0, sizeof(vrng));
    vrng.queue_num = VQUEUE_SIZE;
    vrng.ps_ready = plat_caps()->rng;
}

static void vrng_process_queue(uint8_t *ram) {
    if (!vrng.queue_ready || !vrng.queue_desc_lo) return;

    uint8_t *desc_table = guest_ptr(ram, vrng.queue_desc_lo, VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, vrng.queue_driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, vrng.queue_device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    uint16_t avail_idx;
    avail_idx = g_ld16(avail_ring + 2);

    while (vrng.last_avail_idx != avail_idx) {
        uint16_t ring_pos = vrng.last_avail_idx % VQUEUE_SIZE;
        uint16_t head;
        head = g_ld16(avail_ring + 4u + ring_pos * 2u);
        vrng.last_avail_idx++;

        uint64_t a; uint32_t l; uint16_t f, nx;
        vblk_read_desc(desc_table, head, &a, &l, &f, &nx);
        uint8_t *buf = guest_ptr(ram, (uint32_t)a, l);
        uint32_t filled = 0;
        if (buf && l) {
            if (!plat_random(buf, l)) {
                /* Never leave the guest hanging: a console with no CSPRNG, or
                   one whose service failed, still gets bytes. */
                for (uint32_t i = 0; i < l; i++) buf[i] = (uint8_t)plat_us();
            }
            filled = l;
        }

        uint16_t used_idx_val;
        used_idx_val = g_ld16(used_ring + 2);
        uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
        uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
        uint32_t hd32 = head;
        g_st32(ue + 0, hd32);
        g_st32(ue + 4, filled);
        used_idx_val++;
        g_st16(used_ring + 2, used_idx_val);

        vrng.int_status |= 1u;
    }

    if (vrng.int_status) plic_set_pending(PLIC_SRC_RNG, true);
}

static uint32_t vrng_load(uint32_t addy) {
    uint32_t r = addy - VIRTIO_RNG_BASE;
    switch (r) {
    case VREG_MAGIC:           return 0x74726976u;
    case VREG_VERSION:         return 2u;
    case VREG_DEVICE_ID:       return 4u; /* entropy source */
    case VREG_VENDOR_ID:       return 0x554d4551u;
    case VREG_DEVICE_FEATURES: return (vrng.dev_feat_sel == 1) ? VIRTIO_F_VERSION_1_HI : 0u;
    case VREG_QUEUE_NUM_MAX:   return VQUEUE_SIZE;
    case VREG_QUEUE_READY:     return vrng.queue_ready;
    case VREG_INT_STATUS:      return vrng.int_status;
    case VREG_STATUS:          return vrng.status;
    case VREG_CONFIG_GEN:      return 0u;
    default:                   return 0u;
    }
}

static void vrng_store(uint32_t addy, uint32_t val, uint8_t *ram) {
    uint32_t r = addy - VIRTIO_RNG_BASE;
    switch (r) {
    case VREG_DEV_FEAT_SEL:   vrng.dev_feat_sel = val; break;
    case VREG_DRV_FEAT_SEL:   vrng.drv_feat_sel = val; break;
    case VREG_DRIVER_FEATURES: break;
    case VREG_QUEUE_SEL:      break; /* only queue 0 */
    case VREG_QUEUE_NUM:      vrng.queue_num = val < VQUEUE_SIZE ? val : VQUEUE_SIZE; break;
    case VREG_QUEUE_READY:    vrng.queue_ready = val; break;
    case VREG_QUEUE_NOTIFY:   vrng_process_queue(ram); break;
    case VREG_INT_ACK:
        vrng.int_status &= ~val;
        plic_set_pending(PLIC_SRC_RNG, vrng.int_status != 0);
        break;
    case VREG_STATUS:
        vrng.status = val;
        if (val == 0) {
            vrng.queue_ready = 0;
            vrng.int_status = 0;
            vrng.last_avail_idx = 0;
            plic_set_pending(PLIC_SRC_RNG, false);
        }
        break;
    case VREG_QUEUE_DESC_LO:   vrng.queue_desc_lo   = val; break;
    case VREG_QUEUE_DESC_HI:   break;
    case VREG_QUEUE_DRIVER_LO: vrng.queue_driver_lo = val; break;
    case VREG_QUEUE_DRIVER_HI: break;
    case VREG_QUEUE_DEVICE_LO: vrng.queue_device_lo = val; break;
    case VREG_QUEUE_DEVICE_HI: break;
    default: break;
    }
}
