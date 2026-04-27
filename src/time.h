/* Read RISC-V mtime via the unprivileged `time` CSR. RVVM ticks at
 * 10 MHz by default (RVVM_OPT_TIME_FREQ in src/rvvm.c), so each tick
 * is 100 ns and 1/60 of a second is 166666 ticks. */

#pragma once
#include <stdint.h>

#define TIME_HZ            10000000ULL
#define TICKS_PER_FRAME    (TIME_HZ / 60)

static inline uint64_t time_now(void) {
    uint64_t v;
    __asm__ volatile ("rdtime %0" : "=r"(v));
    return v;
}

static inline void time_busy_until(uint64_t deadline) {
    while (time_now() < deadline) { __asm__ volatile ("nop"); }
}
