#include "uart.h"
#include "time.h"
#include "chip8.h"
#include "bochs.h"
#include "roms.h"

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

/* DTB header (libfdt fdt.h equivalent) — first 40 bytes of every
 * Flattened Device Tree blob. All fields are big-endian uint32s. */
struct fdt_header {
    uint32_t magic;             /* 0xD00DFEED */
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC 0xD00DFEEDU

static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16)
         | (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init();
    uart_puts("\nscev-cores/chip-8 — bare-metal CHIP-8 on RVVM\n");
    uart_printf("hartid=%u  fdt=%p  sp=...  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* Sanity-check the FDT pointer. */
    if (fdt_addr) {
        struct fdt_header *fh = (struct fdt_header *)(uintptr_t)fdt_addr;
        uint32_t magic = be32(fh->magic);
        if (magic == FDT_MAGIC) {
            uart_printf("FDT: magic OK, total %u bytes, version %u\n",
                        (uint64_t)be32(fh->totalsize),
                        (uint64_t)be32(fh->version));
        } else {
            uart_printf("FDT: bad magic %x at %p — not a DTB\n",
                        (uint64_t)magic, (void *)(uintptr_t)fdt_addr);
        }
    } else {
        uart_puts("FDT: a1 was 0 — RVVM didn't pass a DTB?\n");
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
