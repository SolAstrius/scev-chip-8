/* CHIP-8 interpreter — Cowgod's Technical Reference v1.0 conformant.
 *
 * Reference: cowgod's "Chip-8 Technical Reference v1.0"
 * Sections covered: 2.x (specs) + 3.1 (35 standard opcodes).
 * Super-CHIP (3.2) and XO-CHIP extensions are NOT implemented. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define CHIP8_MEM_SIZE      4096
#define CHIP8_PROGRAM_BASE  0x200
#define CHIP8_FONT_BASE     0x050
#define CHIP8_FONT_SIZE     (16 * 5)
#define CHIP8_DISPLAY_W     64
#define CHIP8_DISPLAY_H     32
#define CHIP8_DISPLAY_BYTES (CHIP8_DISPLAY_W * CHIP8_DISPLAY_H)
#define CHIP8_STACK_DEPTH   16

typedef struct {
    /* Memory: 4 KiB flat, fonts at 0x050, programs at 0x200. */
    uint8_t  mem[CHIP8_MEM_SIZE];

    /* Registers. V0-VE general, VF flag. I is 16-bit but only 12 used. */
    uint8_t  v[16];
    uint16_t i;
    uint16_t pc;

    /* Stack — 16 levels of 16-bit return addresses. */
    uint16_t stack[CHIP8_STACK_DEPTH];
    uint8_t  sp;

    /* Timers — decremented at 60 Hz when nonzero. */
    uint8_t  dt;            /* delay timer */
    uint8_t  st;            /* sound timer (beep while > 0) */

    /* Display: byte-per-pixel for clarity. 0 = off, 1 = on. */
    uint8_t  fb[CHIP8_DISPLAY_BYTES];
    bool     fb_dirty;

    /* Input: 16-bit keymask, bit n = key n pressed. */
    uint16_t keys;

    /* Fx0A blocks until any key is pressed; we set this and a target
     * register when Fx0A is decoded, and resolve in chip8_set_key(). */
    bool     waiting_for_key;
    uint8_t  key_dest;

    /* COSMAC VIP "display wait" quirk: DRW stalls the CPU until the
     * next 60 Hz vblank tick. Naturally clusters all sprite ops to
     * one-per-frame, which trades some flicker for fps deterministic
     * scanout pacing. Default OFF; toggle with chip8_set_vblank_wait().
     *
     * Implementation: set vblank_pending=true on DRW; chip8_step
     * returns early while it's set; chip8_tick_60hz clears it. */
    bool     vblank_wait_quirk;
    bool     vblank_pending;

    /* RNG state for Cxkk (xorshift32 — small, sufficient for games). */
    uint32_t rng;

    /* Halt flag — set if we hit an unrecognised opcode or out-of-bounds. */
    bool     halted;
    uint16_t halt_pc;
    uint16_t halt_op;
} chip8_t;

/* Initialise/reset VM state and load font sprites. */
void chip8_reset(chip8_t *vm, uint64_t rng_seed);

/* Toggle the COSMAC-VIP display-wait quirk (default off after reset). */
void chip8_set_vblank_wait(chip8_t *vm, bool enabled);

/* Copy `len` program bytes into mem[0x200..]. Truncates if too large. */
void chip8_load(chip8_t *vm, const uint8_t *prog, uint64_t len);

/* Execute one instruction (advancing PC). No-op if halted or waiting. */
void chip8_step(chip8_t *vm);

/* Decrement DT/ST. Call at 60 Hz from a frame-paced loop. */
void chip8_tick_60hz(chip8_t *vm);

/* Press/release a hex-keypad key (0..15). */
void chip8_set_key(chip8_t *vm, uint8_t key, bool pressed);

/* Render the framebuffer to UART using ANSI-positioned 2-char cells.
 * Sets fb_dirty=false when done. */
void chip8_render_ascii(chip8_t *vm);

/* Render the framebuffer onto a Bochs framebuffer at the given scale
 * (e.g. scale=10 → 640×320). Centred in the display via x_off/y_off.
 * Sets fb_dirty=false when done. */
struct bochs_s;
void chip8_render_bochs(chip8_t *vm, const struct bochs_s *bd,
                        uint32_t scale, uint32_t x_off, uint32_t y_off,
                        uint32_t fg, uint32_t bg);
