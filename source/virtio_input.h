#pragma once
#include <stdint.h>
#include <string.h>
#include <3ds.h>

/*
 * Virtio-input device: the 3DS's motion sensors and sliders as a normal
 * Linux evdev node (/dev/input/event0).
 *
 * These four - accelerometer, gyroscope, 3D slider, volume slider - are the
 * only 3DS inputs that map cleanly onto an existing guest driver, so they
 * get a real one. Everything else the console can report is a text file in
 * the `hw` 9P tree instead (see hw3ds.h for why). The sensors appear in
 * both places on purpose: evdev for programs that speak it, and
 * /mnt/hw/accel for a shell script that just wants a number.
 *
 * Two virtqueues, per the virtio-input spec: queue 0 (eventq) is where the
 * driver parks writable buffers for the device to fill with input_event
 * records, and queue 1 (statusq) carries LED/force-feedback writes back to
 * the device - nothing here has either, so it's drained and ignored.
 *
 * Sampling is driven from the main loop rather than from a timer: the
 * emulator is already a cooperative single thread, and hidScanInput() is
 * called there every frame anyway, so vinput_poll() is nearly free and
 * naturally rate-limits to the 3DS's own refresh.
 */

#define VIRTIO_INPUT_BASE  0x10007000u
#define VIRTIO_INPUT_SIZE  0x1000u

/* config select values */
#define VI_CFG_UNSET      0x00
#define VI_CFG_ID_NAME    0x01
#define VI_CFG_ID_SERIAL  0x02
#define VI_CFG_ID_DEVIDS  0x03
#define VI_CFG_PROP_BITS  0x10
#define VI_CFG_EV_BITS    0x11
#define VI_CFG_ABS_INFO   0x12

/* evdev event types / codes (guest-side ABI values) */
#define EV_SYN   0x00
#define EV_ABS   0x03
#define SYN_REPORT 0x00

#define ABS_X       0x00
#define ABS_Y       0x01
#define ABS_Z       0x02
#define ABS_RX      0x03
#define ABS_RY      0x04
#define ABS_RZ      0x05
#define ABS_VOLUME  0x20
#define ABS_MISC    0x28

#define VI_NAXES 8

/* Axis table. The accelerometer and gyroscope report signed 16-bit counts
   straight from HID shared memory - no scaling, so a guest reading them
   sees exactly what Horizon does. The two sliders are rescaled to the
   ranges they're naturally read in (percent, and the volume slider's own
   0-63 hardware steps). */
static const struct { uint16_t code; int32_t min, max; } vi_axes[VI_NAXES] = {
    { ABS_X,      -32768, 32767 },
    { ABS_Y,      -32768, 32767 },
    { ABS_Z,      -32768, 32767 },
    { ABS_RX,     -32768, 32767 },
    { ABS_RY,     -32768, 32767 },
    { ABS_RZ,     -32768, 32767 },
    { ABS_MISC,        0,   100 },   /* 3D slider, percent */
    { ABS_VOLUME,      0,    63 },   /* volume slider */
};

typedef struct {
    uint32_t ready, desc_lo, driver_lo, device_lo;
    uint16_t last_avail;
} vi_queue;

static struct {
    uint32_t dev_feat_sel, drv_feat_sel;
    uint32_t queue_sel, queue_num;
    uint32_t int_status, status;
    uint8_t  cfg_select, cfg_subsel;
    vi_queue q[2];
    int32_t  last[VI_NAXES];
    bool     have_last;
} vin;

static void vinput_init(void) {
    memset(&vin, 0, sizeof(vin));
    vin.queue_num = VQUEUE_SIZE;
}

/* ------------------------------------------------------------------
 * Config space
 * ------------------------------------------------------------------ */

/* Builds the union payload for the currently selected config item.
   Returns its length, which is what the driver reads from the `size`
   byte at config offset 2. A length of 0 is the spec's way of saying
   "this device has nothing of that kind". */
static uint32_t vinput_cfg_data(uint8_t *out) {
    switch (vin.cfg_select) {
    case VI_CFG_ID_NAME: {
        const char *n = "Nintendo 3DS motion sensors";
        uint32_t l = (uint32_t)strlen(n);
        memcpy(out, n, l);
        return l;
    }
    case VI_CFG_ID_SERIAL: {
        const char *n = "3ds-cli";
        uint32_t l = (uint32_t)strlen(n);
        memcpy(out, n, l);
        return l;
    }
    case VI_CFG_ID_DEVIDS: {
        /* bustype BUS_VIRTUAL(0x06), then vendor/product/version. */
        uint16_t ids[4] = { 0x06, 0x057e /* Nintendo */, 0x3d5, 1 };
        memcpy(out, ids, 8);
        return 8;
    }
    case VI_CFG_EV_BITS:
        if (vin.cfg_subsel == EV_ABS) {
            uint32_t maxbit = 0;
            for (int i = 0; i < VI_NAXES; i++)
                if (vi_axes[i].code > maxbit) maxbit = vi_axes[i].code;
            uint32_t nbytes = maxbit / 8 + 1;
            memset(out, 0, nbytes);
            for (int i = 0; i < VI_NAXES; i++)
                out[vi_axes[i].code / 8] |= (uint8_t)(1u << (vi_axes[i].code % 8));
            return nbytes;
        }
        /* EV_SYN is implied by the presence of any other event type, and
           the driver only checks for a non-zero size here. */
        return 0;

    case VI_CFG_ABS_INFO:
        for (int i = 0; i < VI_NAXES; i++) {
            if (vi_axes[i].code != vin.cfg_subsel) continue;
            uint32_t v[5] = { (uint32_t)vi_axes[i].min, (uint32_t)vi_axes[i].max,
                              0 /* fuzz */, 0 /* flat */, 0 /* res */ };
            memcpy(out, v, sizeof(v));
            return sizeof(v);
        }
        return 0;

    case VI_CFG_PROP_BITS:
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------
 * Event injection
 * ------------------------------------------------------------------ */

/* Pushes one input_event into the eventq. Returns false when the driver
   hasn't left a buffer for us, in which case the sample is dropped - the
   next poll carries the current value anyway, so a stale queue can never
   make the guest's view lag reality. */
static bool vinput_push(uint8_t *ram, uint16_t type, uint16_t code, int32_t value) {
    vi_queue *q = &vin.q[0];
    if (!q->ready || !q->desc_lo) return false;

    uint8_t *desc_table = guest_ptr(ram, q->desc_lo, VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, q->driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, q->device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return false;

    uint16_t avail_idx;
    memcpy(&avail_idx, avail_ring + 2, 2);
    if (q->last_avail == avail_idx) return false;   /* no free buffer */

    uint16_t ring_pos = q->last_avail % VQUEUE_SIZE;
    uint16_t head;
    memcpy(&head, avail_ring + 4u + ring_pos * 2u, 2);
    q->last_avail++;

    uint64_t a; uint32_t l; uint16_t f, nx;
    vblk_read_desc(desc_table, head, &a, &l, &f, &nx);
    uint8_t *buf = guest_ptr(ram, (uint32_t)a, l);

    uint32_t written = 0;
    if (buf && l >= 8) {
        memcpy(buf + 0, &type,  2);
        memcpy(buf + 2, &code,  2);
        memcpy(buf + 4, &value, 4);
        written = 8;
    }

    uint16_t used_idx_val;
    memcpy(&used_idx_val, used_ring + 2, 2);
    uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
    uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
    uint32_t hd32 = head;
    memcpy(ue + 0, &hd32, 4);
    memcpy(ue + 4, &written, 4);
    used_idx_val++;
    memcpy(used_ring + 2, &used_idx_val, 2);

    vin.int_status |= 1u;
    return true;
}

/* Samples all axes and emits events for the ones that moved, followed by a
   SYN_REPORT if anything did. Called from the main loop. */
static void vinput_poll(uint8_t *ram) {
    if (!vin.q[0].ready) return;

    accelVector av = {0, 0, 0};
    angularRate gr = {0, 0, 0};
    if (hw.sensors) { hidAccelRead(&av); hidGyroRead(&gr); }

    /* accel/gyro come straight out of HID shared memory, so reading them
       every frame is free. The volume slider needs an actual IPC round trip
       to the HID service, and it's a physical slider nobody moves 60 times
       a second, so it's sampled far more rarely. */
    static int slider_div = 0;
    static u8  vol = 0;
    if (slider_div-- <= 0) {
        slider_div = 30;
        if (R_FAILED(HIDUSER_GetSoundVolume(&vol))) vol = 0;
    }

    int32_t cur[VI_NAXES] = {
        av.x, av.y, av.z,
        gr.x, gr.y, gr.z,
        (int32_t)(osGet3DSliderState() * 100.0f + 0.5f),
        (int32_t)vol,
    };

    bool any = false;
    for (int i = 0; i < VI_NAXES; i++) {
        if (vin.have_last && cur[i] == vin.last[i]) continue;
        if (!vinput_push(ram, EV_ABS, vi_axes[i].code, cur[i])) break;
        vin.last[i] = cur[i];
        any = true;
    }
    vin.have_last = true;

    if (any) {
        vinput_push(ram, EV_SYN, SYN_REPORT, 0);
        plic_set_pending(PLIC_SRC_INPUT, true);
    }
}

/* The driver posts LED/FF status here. Nothing on a 3DS consumes them, so
   buffers are completed immediately to keep the queue from filling up. */
static void vinput_drain_statusq(uint8_t *ram) {
    vi_queue *q = &vin.q[1];
    if (!q->ready || !q->desc_lo) return;
    uint8_t *desc_table = guest_ptr(ram, q->desc_lo, VQUEUE_SIZE * 16u);
    uint8_t *avail_ring = guest_ptr(ram, q->driver_lo, 6u);
    uint8_t *used_ring  = guest_ptr(ram, q->device_lo, 6u);
    if (!desc_table || !avail_ring || !used_ring) return;

    uint16_t avail_idx;
    memcpy(&avail_idx, avail_ring + 2, 2);
    while (q->last_avail != avail_idx) {
        uint16_t ring_pos = q->last_avail % VQUEUE_SIZE;
        uint16_t head;
        memcpy(&head, avail_ring + 4u + ring_pos * 2u, 2);
        q->last_avail++;

        uint16_t used_idx_val;
        memcpy(&used_idx_val, used_ring + 2, 2);
        uint16_t used_slot = used_idx_val % VQUEUE_SIZE;
        uint8_t *ue = used_ring + 4u + (uint32_t)used_slot * 8u;
        uint32_t hd32 = head, zero = 0;
        memcpy(ue + 0, &hd32, 4);
        memcpy(ue + 4, &zero, 4);
        used_idx_val++;
        memcpy(used_ring + 2, &used_idx_val, 2);
        vin.int_status |= 1u;
    }
    if (vin.int_status) plic_set_pending(PLIC_SRC_INPUT, true);
}

/* ------------------------------------------------------------------
 * MMIO
 * ------------------------------------------------------------------ */

static uint32_t vinput_load(uint32_t addy) {
    uint32_t r = addy - VIRTIO_INPUT_BASE;

    if (r >= 0x100) {
        uint32_t o = r - 0x100;
        uint8_t data[128];
        uint32_t len = vinput_cfg_data(data);
        if (o == 0) return vin.cfg_select;
        if (o == 1) return vin.cfg_subsel;
        if (o == 2) return len;
        if (o >= 8 && o - 8 < len) return data[o - 8];
        return 0;
    }

    switch (r) {
    case VREG_MAGIC:           return 0x74726976u;
    case VREG_VERSION:         return 2u;
    case VREG_DEVICE_ID:       return 18u;  /* input device */
    case VREG_VENDOR_ID:       return 0x554d4551u;
    case VREG_DEVICE_FEATURES: return (vin.dev_feat_sel == 1) ? VIRTIO_F_VERSION_1_HI : 0u;
    case VREG_QUEUE_NUM_MAX:   return VQUEUE_SIZE;
    case VREG_QUEUE_READY:     return (vin.queue_sel < 2) ? vin.q[vin.queue_sel].ready : 0u;
    case VREG_INT_STATUS:      return vin.int_status;
    case VREG_STATUS:          return vin.status;
    case VREG_CONFIG_GEN:      return 0u;
    default:                   return 0u;
    }
}

static void vinput_store(uint32_t addy, uint32_t val, uint8_t *ram) {
    uint32_t r = addy - VIRTIO_INPUT_BASE;

    /* Config writes are how the driver walks the device's capabilities:
       it sets select/subsel, then reads back size and the payload. */
    if (r >= 0x100) {
        uint32_t o = r - 0x100;
        if (o == 0) vin.cfg_select = (uint8_t)val;
        else if (o == 1) vin.cfg_subsel = (uint8_t)val;
        return;
    }

    vi_queue *q = (vin.queue_sel < 2) ? &vin.q[vin.queue_sel] : NULL;
    switch (r) {
    case VREG_DEV_FEAT_SEL:    vin.dev_feat_sel = val; break;
    case VREG_DRV_FEAT_SEL:    vin.drv_feat_sel = val; break;
    case VREG_DRIVER_FEATURES: break;
    case VREG_QUEUE_SEL:       vin.queue_sel = val; break;
    case VREG_QUEUE_NUM:       vin.queue_num = val < VQUEUE_SIZE ? val : VQUEUE_SIZE; break;
    case VREG_QUEUE_READY:     if (q) q->ready = val; break;
    case VREG_QUEUE_NOTIFY:
        /* Only the statusq needs servicing on notify; eventq buffers are
           consumed by vinput_poll() as samples arrive. */
        if (val == 1) vinput_drain_statusq(ram);
        break;
    case VREG_INT_ACK:
        vin.int_status &= ~val;
        plic_set_pending(PLIC_SRC_INPUT, vin.int_status != 0);
        break;
    case VREG_STATUS:
        vin.status = val;
        if (val == 0) {
            for (int i = 0; i < 2; i++) {
                vin.q[i].ready = 0;
                vin.q[i].last_avail = 0;
            }
            vin.int_status = 0;
            vin.have_last = false;
            plic_set_pending(PLIC_SRC_INPUT, false);
        }
        break;
    case VREG_QUEUE_DESC_LO:   if (q) q->desc_lo   = val; break;
    case VREG_QUEUE_DRIVER_LO: if (q) q->driver_lo = val; break;
    case VREG_QUEUE_DEVICE_LO: if (q) q->device_lo = val; break;
    default: break;
    }
}
