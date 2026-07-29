#pragma once
#include <stdint.h>
#include <3ds.h>

/*
 * Goldfish RTC — a tiny, well-known MMIO real-time-clock device
 * (drivers/rtc/rtc-goldfish.c in Linux, DT binding "google,goldfish-rtc").
 * Two 32-bit registers: reading TIME_LOW latches the current time and
 * TIME_HIGH returns the latched high half, giving the guest a monotonic
 * 64-bit nanosecond-since-epoch read. We back it with the 3DS's own
 * battery-backed hardware RTC (osGetTime(), ms since epoch), so the guest
 * boots with the real date/time instead of the kernel default (1970).
 */

#define RTC_GOLDFISH_BASE 0x10004000u
#define RTC_GOLDFISH_SIZE 0x1000u

#define RTC_REG_TIME_LOW  0x00
#define RTC_REG_TIME_HIGH 0x04

static uint64_t rtc_latched_ns = 0;

static uint32_t rtc_goldfish_load(uint32_t addy) {
    uint32_t r = addy - RTC_GOLDFISH_BASE;
    switch (r) {
    case RTC_REG_TIME_LOW: {
        uint64_t ms = osGetTime(); /* real wall-clock ms since epoch */
        rtc_latched_ns = ms * 1000000ull;
        return (uint32_t)(rtc_latched_ns & 0xffffffffu);
    }
    case RTC_REG_TIME_HIGH:
        return (uint32_t)(rtc_latched_ns >> 32);
    default:
        return 0u;
    }
}

static void rtc_goldfish_store(uint32_t addy, uint32_t val) {
    (void)addy; (void)val; /* alarm/interrupt regs unused; read-only clock */
}
