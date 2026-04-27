#include "uart.h"
#include "time.h"
#include "chip8.h"
#include "roms.h"

extern char __bss_start[], __bss_end[];

/* CHIP-8 standard cycle rate is poorly defined in the spec — the COSMAC
 * VIP ran at ~500–700 cycles/sec. Most modern games target ~10 cycles
 * per 60 Hz frame; bump higher for compute-heavy programs. */
#define CYCLES_PER_FRAME    11

/* Single static instance — no allocator yet. */
static chip8_t vm;

static void map_host_to_chip8_keys(int c, bool down) {
    /* Match the canonical 4×4 mapping (left half of QWERTY keyboard):
     *   1 2 3 4   →   1 2 3 C
     *   q w e r   →   4 5 6 D
     *   a s d f   →   7 8 9 E
     *   z x c v   →   A 0 B F   */
    static const struct { char c; uint8_t k; } map[] = {
        {'1', 0x1}, {'2', 0x2}, {'3', 0x3}, {'4', 0xC},
        {'q', 0x4}, {'w', 0x5}, {'e', 0x6}, {'r', 0xD},
        {'a', 0x7}, {'s', 0x8}, {'d', 0x9}, {'f', 0xE},
        {'z', 0xA}, {'x', 0x0}, {'c', 0xB}, {'v', 0xF},
    };
    for (uint64_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].c == c) { chip8_set_key(&vm, map[i].k, down); return; }
    }
}

void kmain(void) {
    uart_init();
    uart_puts("\x1b[2J\x1b[H");                         /* clear + home */
    uart_puts("scev-cores/chip-8 — bare-metal CHIP-8 on RVVM\n");
    uart_printf("BSS: %u bytes; t0=%u ticks (10 MHz)\n",
                (uint64_t)(__bss_end - __bss_start),
                (uint64_t)time_now());

    chip8_reset(&vm, time_now());
    chip8_load(&vm, rom_ibm_logo, sizeof(rom_ibm_logo));
    uart_printf("loaded IBM logo (%u bytes), pc=%x\n",
                (uint64_t)sizeof(rom_ibm_logo), (uint64_t)vm.pc);

    uart_puts("\nKeypad: 1234 / qwer / asdf / zxcv\n");
    uart_puts("Quit RVVM with Ctrl-A x.\n\n");

    /* Frame-paced main loop. Each iteration = 1/60 s. */
    uint64_t deadline = time_now() + TICKS_PER_FRAME;
    /* Track key state so we can emit release events on a simple
     * "no-input-this-frame" decay. RVVM forwards stdin chars one at a
     * time without separate up/down events, so we time-out keys after
     * a few frames of silence to fake releases. */
    uint8_t  key_age[16] = {0};

    for (;;) {
        /* Drain available input. Each char marks its mapped key fresh. */
        for (int c; (c = uart_getc_nb()) >= 0; ) {
            map_host_to_chip8_keys(c, true);
            for (int i = 0; i < 16; i++) {
                if ((vm.keys >> i) & 1) key_age[i] = 6;     /* ~100 ms */
            }
        }

        /* Run CPU cycles. */
        for (int i = 0; i < CYCLES_PER_FRAME && !vm.halted; i++) {
            chip8_step(&vm);
        }

        /* Tick 60 Hz timers. */
        chip8_tick_60hz(&vm);

        /* Decay keys. */
        for (int i = 0; i < 16; i++) {
            if (key_age[i] && --key_age[i] == 0) chip8_set_key(&vm, i, false);
        }

        /* Repaint if anything changed. */
        if (vm.fb_dirty) chip8_render_ascii(&vm);

        if (vm.halted) {
            uart_puts("\nVM halted. Spinning forever.\n");
            for (;;) __asm__ volatile ("wfi");
        }

        /* Frame pacing. */
        time_busy_until(deadline);
        deadline += TICKS_PER_FRAME;
    }
}
