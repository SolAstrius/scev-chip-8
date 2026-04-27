#include "chip8.h"
#include "uart.h"
#include "gfx.h"

/* Hex-digit font — 5 bytes per glyph, 16 glyphs, total 80 bytes. From
 * Cowgod section 2.4. Bit 7 = leftmost pixel; only top 4 bits drawn
 * (sprite is 8 wide but glyphs are 4 wide, low nibble is always 0). */
static const uint8_t hex_font[CHIP8_FONT_SIZE] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0,  /* 0 */
    0x20, 0x60, 0x20, 0x20, 0x70,  /* 1 */
    0xF0, 0x10, 0xF0, 0x80, 0xF0,  /* 2 */
    0xF0, 0x10, 0xF0, 0x10, 0xF0,  /* 3 */
    0x90, 0x90, 0xF0, 0x10, 0x10,  /* 4 */
    0xF0, 0x80, 0xF0, 0x10, 0xF0,  /* 5 */
    0xF0, 0x80, 0xF0, 0x90, 0xF0,  /* 6 */
    0xF0, 0x10, 0x20, 0x40, 0x40,  /* 7 */
    0xF0, 0x90, 0xF0, 0x90, 0xF0,  /* 8 */
    0xF0, 0x90, 0xF0, 0x10, 0xF0,  /* 9 */
    0xF0, 0x90, 0xF0, 0x90, 0x90,  /* A */
    0xE0, 0x90, 0xE0, 0x90, 0xE0,  /* B */
    0xF0, 0x80, 0x80, 0x80, 0xF0,  /* C */
    0xE0, 0x90, 0x90, 0x90, 0xE0,  /* D */
    0xF0, 0x80, 0xF0, 0x80, 0xF0,  /* E */
    0xF0, 0x80, 0xF0, 0x80, 0x80,  /* F */
};

void chip8_reset(chip8_t *vm, uint64_t rng_seed) {
    /* Zero everything. */
    uint8_t *p = (uint8_t *)vm;
    for (uint64_t i = 0; i < sizeof(*vm); i++) p[i] = 0;

    /* Install font at the canonical address. */
    for (uint64_t i = 0; i < CHIP8_FONT_SIZE; i++) {
        vm->mem[CHIP8_FONT_BASE + i] = hex_font[i];
    }

    vm->pc       = CHIP8_PROGRAM_BASE;
    vm->fb_dirty = true;

    /* Seed RNG; xorshift32 forbids zero. */
    vm->rng = (uint32_t)rng_seed;
    if (vm->rng == 0) vm->rng = 0xCAFEF00D;
}

void chip8_tick_60hz(chip8_t *vm);     /* fwd to keep order tidy */

void chip8_load(chip8_t *vm, const uint8_t *prog, uint64_t len) {
    uint64_t cap = CHIP8_MEM_SIZE - CHIP8_PROGRAM_BASE;
    if (len > cap) len = cap;
    for (uint64_t i = 0; i < len; i++) {
        vm->mem[CHIP8_PROGRAM_BASE + i] = prog[i];
    }
}

void chip8_tick_60hz(chip8_t *vm) {
    if (vm->dt) vm->dt--;
    if (vm->st) vm->st--;
    vm->vblank_pending = false;            /* release any DRW stall */
}

void chip8_set_key(chip8_t *vm, uint8_t key, bool pressed) {
    key &= 0xF;
    uint16_t mask = (uint16_t)(1u << key);
    if (pressed) vm->keys |= mask;
    else         vm->keys &= (uint16_t)~mask;

    /* Resolve a pending Fx0A wait on key down. */
    if (pressed && vm->waiting_for_key) {
        vm->v[vm->key_dest]   = key;
        vm->waiting_for_key   = false;
    }
}

static uint8_t rng_next(chip8_t *vm) {
    uint32_t x = vm->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rng = x;
    return (uint8_t)(x & 0xFF);
}

static void halt(chip8_t *vm, uint16_t op) {
    vm->halted  = true;
    vm->halt_pc = vm->pc;
    vm->halt_op = op;
    uart_printf("CHIP-8 halt: pc=%x op=%x\n",
                (uint64_t)vm->pc, (uint64_t)op);
}

/* Dxyn — XOR-draw an N-byte sprite from mem[I] at (Vx, Vy). VF set if
 * any on-pixel was erased. Coordinates wrap modulo display dims; the
 * sprite itself is clipped (not wrapped) on the right/bottom edge per
 * the original COSMAC VIP behavior described by Cowgod. */
static void op_drw(chip8_t *vm, uint8_t x_reg, uint8_t y_reg, uint8_t n) {
    uint8_t x0 = vm->v[x_reg] % CHIP8_DISPLAY_W;
    uint8_t y0 = vm->v[y_reg] % CHIP8_DISPLAY_H;
    vm->v[0xF] = 0;

    for (uint8_t row = 0; row < n; row++) {
        uint8_t y = y0 + row;
        if (y >= CHIP8_DISPLAY_H) break;
        uint8_t bits = vm->mem[(vm->i + row) & 0xFFF];

        for (uint8_t col = 0; col < 8; col++) {
            uint8_t x = x0 + col;
            if (x >= CHIP8_DISPLAY_W) break;
            uint8_t spr = (bits >> (7 - col)) & 1;
            if (!spr) continue;

            uint8_t *px = &vm->fb[y * CHIP8_DISPLAY_W + x];
            if (*px) vm->v[0xF] = 1;        /* collision */
            *px ^= 1;
        }
    }
    vm->fb_dirty = true;
    if (vm->vblank_wait_quirk) vm->vblank_pending = true;
}

void chip8_set_vblank_wait(chip8_t *vm, bool enabled) {
    vm->vblank_wait_quirk = enabled;
    if (!enabled) vm->vblank_pending = false;
}

void chip8_step(chip8_t *vm) {
    if (vm->halted || vm->waiting_for_key) return;
    if (vm->vblank_pending) return;          /* COSMAC display-wait quirk */
    if (vm->pc >= CHIP8_MEM_SIZE - 1) { halt(vm, 0xFFFF); return; }

    /* Fetch a 16-bit big-endian opcode. */
    uint16_t op = ((uint16_t)vm->mem[vm->pc] << 8) | vm->mem[vm->pc + 1];
    vm->pc += 2;

    /* Common operand decodes. */
    uint16_t nnn = op & 0x0FFF;
    uint8_t  n   = op & 0x000F;
    uint8_t  x   = (op >> 8) & 0xF;
    uint8_t  y   = (op >> 4) & 0xF;
    uint8_t  kk  = op & 0xFF;

    switch (op >> 12) {
    case 0x0:
        if      (op == 0x00E0) {                        /* CLS */
            for (uint64_t i = 0; i < CHIP8_DISPLAY_BYTES; i++) vm->fb[i] = 0;
            vm->fb_dirty = true;
        } else if (op == 0x00EE) {                       /* RET */
            if (vm->sp == 0) { halt(vm, op); break; }
            vm->pc = vm->stack[--vm->sp];
        } else {
            /* 0nnn SYS — ignored on modern interpreters. */
        }
        break;

    case 0x1: vm->pc = nnn; break;                      /* JP nnn */

    case 0x2:                                           /* CALL nnn */
        if (vm->sp >= CHIP8_STACK_DEPTH) { halt(vm, op); break; }
        vm->stack[vm->sp++] = vm->pc;
        vm->pc = nnn;
        break;

    case 0x3: if (vm->v[x] == kk)        vm->pc += 2; break;
    case 0x4: if (vm->v[x] != kk)        vm->pc += 2; break;
    case 0x5: if (vm->v[x] == vm->v[y])  vm->pc += 2; break;
    case 0x6: vm->v[x]  = kk;                        break;
    case 0x7: vm->v[x] += kk;                        break;

    case 0x8: {
        uint8_t a = vm->v[x], b = vm->v[y], r = 0;
        switch (n) {
        case 0x0: r = b;                                  vm->v[x] = r; break;
        case 0x1: r = a | b;                              vm->v[x] = r; break;
        case 0x2: r = a & b;                              vm->v[x] = r; break;
        case 0x3: r = a ^ b;                              vm->v[x] = r; break;
        case 0x4: {
            uint16_t s = a + b;
            vm->v[x] = (uint8_t)s;
            vm->v[0xF] = (s > 0xFF) ? 1 : 0;
        } break;
        case 0x5: {
            uint8_t f = (a >= b) ? 1 : 0;          /* NOT borrow */
            vm->v[x] = a - b;
            vm->v[0xF] = f;
        } break;
        case 0x6: {
            uint8_t f = a & 1;                     /* LSB before shift */
            vm->v[x] = a >> 1;
            vm->v[0xF] = f;
        } break;
        case 0x7: {
            uint8_t f = (b >= a) ? 1 : 0;
            vm->v[x] = b - a;
            vm->v[0xF] = f;
        } break;
        case 0xE: {
            uint8_t f = (a >> 7) & 1;              /* MSB before shift */
            vm->v[x] = a << 1;
            vm->v[0xF] = f;
        } break;
        default: halt(vm, op); break;
        }
    } break;

    case 0x9: if (vm->v[x] != vm->v[y])  vm->pc += 2; break;
    case 0xA: vm->i = nnn;                            break;
    case 0xB: vm->pc = (uint16_t)(nnn + vm->v[0]);    break;
    case 0xC: vm->v[x] = rng_next(vm) & kk;           break;
    case 0xD: op_drw(vm, x, y, n);                    break;

    case 0xE:
        if      (kk == 0x9E) { if ( (vm->keys >> (vm->v[x] & 0xF)) & 1) vm->pc += 2; }
        else if (kk == 0xA1) { if (!((vm->keys >> (vm->v[x] & 0xF)) & 1)) vm->pc += 2; }
        else                 { halt(vm, op); }
        break;

    case 0xF:
        switch (kk) {
        case 0x07: vm->v[x] = vm->dt; break;
        case 0x0A: vm->waiting_for_key = true; vm->key_dest = x; break;
        case 0x15: vm->dt = vm->v[x]; break;
        case 0x18: vm->st = vm->v[x]; break;
        case 0x1E: vm->i  = (uint16_t)(vm->i + vm->v[x]); break;
        case 0x29: vm->i  = (uint16_t)(CHIP8_FONT_BASE + (vm->v[x] & 0xF) * 5); break;
        case 0x33: {                              /* BCD into mem[I..I+2] */
            uint8_t v = vm->v[x];
            vm->mem[(vm->i + 0) & 0xFFF] = v / 100;
            vm->mem[(vm->i + 1) & 0xFFF] = (v / 10) % 10;
            vm->mem[(vm->i + 2) & 0xFFF] = v % 10;
        } break;
        case 0x55: for (uint8_t k = 0; k <= x; k++) vm->mem[(vm->i + k) & 0xFFF] = vm->v[k]; break;
        case 0x65: for (uint8_t k = 0; k <= x; k++) vm->v[k] = vm->mem[(vm->i + k) & 0xFFF]; break;
        default:   halt(vm, op); break;
        }
        break;

    default: halt(vm, op); break;
    }
}

void chip8_render_ascii(chip8_t *vm) {
    /* ANSI: cursor home, hide cursor. Each pixel is 2 chars wide so the
     * 64×32 screen lands as roughly square in a typical terminal. */
    uart_puts("\x1b[H\x1b[?25l");

    /* Top border. */
    uart_puts("┌");
    for (int x = 0; x < CHIP8_DISPLAY_W; x++) uart_puts("──");
    uart_puts("┐\n");

    for (int y = 0; y < CHIP8_DISPLAY_H; y++) {
        uart_puts("│");
        for (int x = 0; x < CHIP8_DISPLAY_W; x++) {
            uart_puts(vm->fb[y * CHIP8_DISPLAY_W + x] ? "██" : "  ");
        }
        uart_puts("│\n");
    }

    uart_puts("└");
    for (int x = 0; x < CHIP8_DISPLAY_W; x++) uart_puts("──");
    uart_puts("┘\n");

    vm->fb_dirty = false;
}

void chip8_render_gfx(chip8_t *vm, const gfx_t *g,
                      uint32_t scale, uint32_t x_off, uint32_t y_off,
                      uint32_t fg, uint32_t bg) {
    /* Scale-up nearest-neighbour: each CHIP-8 pixel becomes scale×scale. */
    for (int y = 0; y < CHIP8_DISPLAY_H; y++) {
        for (int x = 0; x < CHIP8_DISPLAY_W; x++) {
            uint32_t c = vm->fb[y * CHIP8_DISPLAY_W + x] ? fg : bg;
            uint32_t px = x_off + (uint32_t)x * scale;
            uint32_t py = y_off + (uint32_t)y * scale;
            gfx_rect(g, px, py, scale, scale, c);
        }
    }
    vm->fb_dirty = false;
}
