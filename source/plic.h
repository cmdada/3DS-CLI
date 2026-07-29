#pragma once
#include <stdint.h>
#include <string.h>

/*
 * Minimal SiFive/RISC-V PLIC (platform-level interrupt controller).
 *
 * Root cause this fixes: virtio-blk, virtio-net and virtio-rng all used to
 * assert the CPU's single external-interrupt line (mip.MEIP) directly, with
 * each device's own ACK handler unconditionally clearing that shared bit.
 * That's architecturally wrong for a system with more than one interrupt
 * source (it's exactly the problem a PLIC exists to solve) and manifested
 * as Linux's irqentry_nmi_enter() nesting counter overflowing and hitting
 * its BUG_ON() — confirmed by tracing pc/mcause through an emulator boot
 * that reliably died in the kernel's M-mode trap entry code.
 *
 * Register layout matches real hardware exactly (see
 * drivers/irqchip/irq-sifive-plic.c): priority array at +0, pending bits at
 * +0x1000, per-context enable bits at +0x2000, per-context threshold/claim
 * at +0x200000. Only what a single-hart, single-context (S-mode) guest
 * actually uses is implemented; multi-hart contexts are irrelevant here.
 */

#define PLIC_BASE        0x10010000u
#define PLIC_SIZE        0x201000u
#define PLIC_NDEV        8u    /* interrupt sources 1..PLIC_NDEV are valid */

#define PLIC_SRC_BLK   1u
#define PLIC_SRC_NET   2u
#define PLIC_SRC_RNG   3u

#define PLIC_PRIORITY_BASE   0x000000u
#define PLIC_PENDING_BASE    0x001000u
#define PLIC_ENABLE_BASE     0x002000u
#define PLIC_ENABLE_SIZE     0x80u
#define PLIC_CONTEXT_BASE    0x200000u
#define PLIC_CONTEXT_SIZE    0x1000u
#define PLIC_CONTEXT_THRESHOLD 0x00u
#define PLIC_CONTEXT_CLAIM     0x04u

/* mip bit 9 = SEIP (supervisor external interrupt pending) — the PLIC's
   single output line into the CPU. Devices no longer touch this bit
   directly; they go through plic_set_pending() instead.
   The guest kernel now boots directly in S-mode (see
   vendor/mini-rv32ima-mmu's file header), so the PLIC only needs to
   expose the S-mode context (context 0 below); there's no M-mode context
   to speak of since M-mode never runs any guest code in this design. */
#ifndef MIP_SEIP
#define MIP_SEIP (1u << 9)
#endif

static struct {
    uint32_t priority[PLIC_NDEV + 1];
    uint32_t pending;     /* bit i = source i asserted & not yet claimed */
    uint32_t enable;      /* bit i = source i enabled for context 0 */
    uint32_t threshold;
} plic;

static void plic_recompute_seip(void) {
    uint32_t claimable = plic.pending & plic.enable;
    bool any = false;
    for (uint32_t i = 1; i <= PLIC_NDEV; i++) {
        if ((claimable & (1u << i)) && plic.priority[i] > plic.threshold) { any = true; break; }
    }
    if (any) core->mip |= MIP_SEIP;
    else core->mip &= ~MIP_SEIP;
}

/* Devices call this whenever their own asserted/cleared state changes. */
static void plic_set_pending(uint32_t source, bool asserted) {
    if (source < 1 || source > PLIC_NDEV) return;
    if (asserted) plic.pending |= (1u << source);
    else plic.pending &= ~(1u << source);
    plic_recompute_seip();
}

static void plic_init(void) {
    memset(&plic, 0, sizeof(plic));
    /* Sensible default priorities so sources are claimable as soon as the
       driver enables them, even before it explicitly writes a priority. */
    for (uint32_t i = 1; i <= PLIC_NDEV; i++) plic.priority[i] = 1;
}

static uint32_t plic_load(uint32_t addy) {
    uint32_t r = addy - PLIC_BASE;

    if (r >= PLIC_PRIORITY_BASE && r < PLIC_PENDING_BASE) {
        uint32_t id = r / 4;
        return (id >= 1 && id <= PLIC_NDEV) ? plic.priority[id] : 0u;
    }
    if (r >= PLIC_PENDING_BASE && r < PLIC_ENABLE_BASE) {
        return (r == PLIC_PENDING_BASE) ? plic.pending : 0u;
    }
    if (r >= PLIC_ENABLE_BASE && r < PLIC_CONTEXT_BASE) {
        uint32_t ctx = (r - PLIC_ENABLE_BASE) / PLIC_ENABLE_SIZE;
        uint32_t off = (r - PLIC_ENABLE_BASE) % PLIC_ENABLE_SIZE;
        return (ctx == 0 && off == 0) ? plic.enable : 0u;
    }
    if (r >= PLIC_CONTEXT_BASE && r < PLIC_SIZE) {
        uint32_t ctx = (r - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_SIZE;
        uint32_t off = (r - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_SIZE;
        if (ctx != 0) return 0u;
        if (off == PLIC_CONTEXT_THRESHOLD) return plic.threshold;
        if (off == PLIC_CONTEXT_CLAIM) {
            /* Claim: pick the highest-priority pending & enabled source,
               clear its pending bit (it's now "in service"), return its id. */
            uint32_t claimable = plic.pending & plic.enable;
            uint32_t best = 0, best_prio = 0;
            for (uint32_t i = 1; i <= PLIC_NDEV; i++) {
                if ((claimable & (1u << i)) && plic.priority[i] > plic.threshold &&
                    plic.priority[i] > best_prio) {
                    best = i; best_prio = plic.priority[i];
                }
            }
            if (best) {
                plic.pending &= ~(1u << best);
                plic_recompute_seip();
            }
            return best;
        }
    }
    return 0u;
}

static void plic_store(uint32_t addy, uint32_t val) {
    uint32_t r = addy - PLIC_BASE;

    if (r >= PLIC_PRIORITY_BASE && r < PLIC_PENDING_BASE) {
        uint32_t id = r / 4;
        if (id >= 1 && id <= PLIC_NDEV) { plic.priority[id] = val; plic_recompute_seip(); }
        return;
    }
    if (r >= PLIC_ENABLE_BASE && r < PLIC_CONTEXT_BASE) {
        uint32_t ctx = (r - PLIC_ENABLE_BASE) / PLIC_ENABLE_SIZE;
        uint32_t off = (r - PLIC_ENABLE_BASE) % PLIC_ENABLE_SIZE;
        if (ctx == 0 && off == 0) { plic.enable = val; plic_recompute_seip(); }
        return;
    }
    if (r >= PLIC_CONTEXT_BASE) {
        uint32_t ctx = (r - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_SIZE;
        uint32_t off = (r - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_SIZE;
        if (ctx != 0) return;
        if (off == PLIC_CONTEXT_THRESHOLD) { plic.threshold = val; plic_recompute_seip(); }
        /* CONTEXT_CLAIM write = "complete": nothing else to do — our
           devices re-assert plic_set_pending() themselves if they still
           have work after being claimed. */
        return;
    }
}
