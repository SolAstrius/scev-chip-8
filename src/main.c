#include "uart.h"
#include "time.h"
#include "chip8.h"
#include "bochs.h"
#include "pci.h"
#include "fdt.h"
#include "roms.h"
#include <stddef.h>     /* NULL */

extern char __bss_start[], __bss_end[];

#define CYCLES_PER_FRAME    11
#define CHIP8_SCALE         10                          /* 64×32 → 640×320 */
#define DISPLAY_W           (CHIP8_DISPLAY_W * CHIP8_SCALE)
#define DISPLAY_H           (CHIP8_DISPLAY_H * CHIP8_SCALE)

#define COLOR_FG            0x00B0FFB0U                 /* CHIP-8 phosphor green */
#define COLOR_BG            0x00101010U                 /* near-black */

static chip8_t vm;
static bochs_t bd;

static void map_host_to_chip8_keys(int c, bool down) {
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

/* Look up `compatible` in FDT and return the first reg.addr, or `fallback`
 * if absent / FDT init failed. */
static uintptr_t fdt_addr_of(const fdt_t *fdt, const char *compat,
                             uintptr_t fallback) {
    uint32_t off = fdt_find_compatible(fdt, compat);
    if (off == UINT32_MAX) return fallback;
    uint64_t addr = 0;
    if (!fdt_node_reg64(fdt, off, 0, &addr, NULL)) return fallback;
    return (uintptr_t)addr;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    /* Bring UART up first with the rvvm.h fallback so we can print
     * diagnostics even if FDT parsing fails. */
    uart_init(0);

    uart_puts("\nscev-cores/chip-8 — bare-metal CHIP-8 on RVVM\n");
    uart_printf("hartid=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* Discover device addresses via FDT, then re-init drivers
     * with the discovered values. */
    fdt_t fdt;
    bool fdt_ok = fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr);
    if (!fdt_ok) {
        uart_puts("FDT: invalid blob — using hardcoded addresses\n");
    } else {
        uintptr_t uart_at = fdt_addr_of(&fdt, "ns16550a",              RVVM_UART_BASE);
        uintptr_t pci_at  = fdt_addr_of(&fdt, "pci-host-ecam-generic", RVVM_PCI_ECAM_BASE);
        uintptr_t i2c_at  = fdt_addr_of(&fdt, "opencores,i2c-ocores",  RVVM_I2C_OC_BASE);
        uintptr_t plic_at = fdt_addr_of(&fdt, "sifive,plic-1.0.0",     0);
        uintptr_t clint_at= fdt_addr_of(&fdt, "sifive,clint0",         0);

        uart_init(uart_at);
        pci_init(pci_at);

        uart_printf("FDT OK — discovered:\n");
        uart_printf("  uart  @ %p\n", (void *)uart_at);
        uart_printf("  pci   @ %p\n", (void *)pci_at);
        uart_printf("  i2c   @ %p\n", (void *)i2c_at);
        uart_printf("  plic  @ %p\n", (void *)plic_at);
        uart_printf("  clint @ %p\n", (void *)clint_at);
    }

    bool gfx = bochs_init(&bd, DISPLAY_W, DISPLAY_H);
    if (gfx) {
        bochs_fill(&bd, COLOR_BG);
        uart_puts("bochs: framebuffer up\n");
    } else {
        uart_puts("bochs: falling back to ANSI UART render\n");
    }

    chip8_reset(&vm, time_now());
    chip8_load(&vm, rom_ibm_logo, sizeof(rom_ibm_logo));
    uart_printf("loaded IBM logo (%u bytes)\n", (uint64_t)sizeof(rom_ibm_logo));

    uart_puts("Keypad: 1234 / qwer / asdf / zxcv (UART input)\n\n");

    uint64_t deadline    = time_now() + RVVM_TICKS_PER_FRAME;
    uint8_t  key_age[16] = {0};

    for (;;) {
        for (int c; (c = uart_getc_nb()) >= 0; ) {
            map_host_to_chip8_keys(c, true);
            for (int i = 0; i < 16; i++) {
                if ((vm.keys >> i) & 1) key_age[i] = 6;
            }
        }

        for (int i = 0; i < CYCLES_PER_FRAME && !vm.halted; i++) {
            chip8_step(&vm);
        }
        chip8_tick_60hz(&vm);

        for (int i = 0; i < 16; i++) {
            if (key_age[i] && --key_age[i] == 0) chip8_set_key(&vm, i, false);
        }

        if (vm.fb_dirty) {
            if (gfx) {
                chip8_render_bochs(&vm, &bd, CHIP8_SCALE, 0, 0, COLOR_FG, COLOR_BG);
            } else {
                chip8_render_ascii(&vm);
            }
        }

        if (vm.halted) {
            uart_puts("\nVM halted. Spinning forever.\n");
            for (;;) __asm__ volatile ("wfi");
        }

        time_busy_until(deadline);
        deadline += RVVM_TICKS_PER_FRAME;
    }
}
