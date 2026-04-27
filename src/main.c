#include "uart.h"
#include "time.h"
#include "chip8.h"
#include "bochs.h"
#include "pci.h"
#include "fdt.h"
#include "i2c.h"
#include "hid.h"
#include "ata.h"
#include "hda.h"
#include "roms.h"
#include <stddef.h>

extern char __bss_start[], __bss_end[];

/* 30 instructions per 60 Hz frame ≈ 1800 ips — matches Octo's default
 * speed for most games, and gives enough headroom that sprite
 * erase→redraw pairs fit in a single frame batch (no inter-frame
 * "sprite missing" flicker). */
#define CYCLES_PER_FRAME    30
#define CHIP8_SCALE         10
#define DISPLAY_W           (CHIP8_DISPLAY_W * CHIP8_SCALE)
#define DISPLAY_H           (CHIP8_DISPLAY_H * CHIP8_SCALE)

#define COLOR_FG            0x00B0FFB0U
#define COLOR_BG            0x00101010U

static chip8_t        vm;
static bochs_t        bd;
static hid_keyboard_t kb;

/* USB HID usage code → CHIP-8 hex keypad. Mapping mirrors the
 * canonical 4×4 layout (left half of QWERTY):
 *   1 2 3 4   →   1 2 3 C
 *   q w e r   →   4 5 6 D
 *   a s d f   →   7 8 9 E
 *   z x c v   →   A 0 B F
 *
 * Returns 0xFF for "not a keypad key." */
static uint8_t hid_to_chip8(uint8_t usage) {
    switch (usage) {
    case 0x1E: return 0x1;     /* 1 */
    case 0x1F: return 0x2;     /* 2 */
    case 0x20: return 0x3;     /* 3 */
    case 0x21: return 0xC;     /* 4 */
    case 0x14: return 0x4;     /* Q */
    case 0x1A: return 0x5;     /* W */
    case 0x08: return 0x6;     /* E */
    case 0x15: return 0xD;     /* R */
    case 0x04: return 0x7;     /* A */
    case 0x16: return 0x8;     /* S */
    case 0x07: return 0x9;     /* D */
    case 0x09: return 0xE;     /* F */
    case 0x1D: return 0xA;     /* Z */
    case 0x1B: return 0x0;     /* X */
    case 0x06: return 0xB;     /* C */
    case 0x19: return 0xF;     /* V */
    default:   return 0xFF;
    }
}

static void on_key_event(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    uint8_t k = hid_to_chip8(usage);
    if (k != 0xFF) chip8_set_key(&vm, k, pressed);
}

/* Look up `compatible` in FDT and return the first reg.addr, or `fallback`. */
static uintptr_t fdt_addr_of(const fdt_t *fdt, const char *compat,
                             uintptr_t fallback) {
    uint32_t off = fdt_find_compatible(fdt, compat);
    if (off == UINT32_MAX) return fallback;
    uint64_t addr = 0;
    if (!fdt_node_reg64(fdt, off, 0, &addr, NULL)) return fallback;
    return (uintptr_t)addr;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\nscev-cores/chip-8 — bare-metal CHIP-8 on RVVM\n");
    uart_printf("hartid=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* FDT discovery + driver re-init with discovered addresses. */
    fdt_t fdt;
    bool fdt_ok = fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr);
    uintptr_t uart_at = fdt_ok ? fdt_addr_of(&fdt, "ns16550a",              RVVM_UART_BASE)
                               : RVVM_UART_BASE;
    uintptr_t pci_at  = fdt_ok ? fdt_addr_of(&fdt, "pci-host-ecam-generic", RVVM_PCI_ECAM_BASE)
                               : RVVM_PCI_ECAM_BASE;
    uintptr_t i2c_at  = fdt_ok ? fdt_addr_of(&fdt, "opencores,i2c-ocores",  RVVM_I2C_OC_BASE)
                               : RVVM_I2C_OC_BASE;
    uart_init(uart_at);
    pci_init(pci_at);
    i2c_init(i2c_at);
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);
    uart_printf("FDT: uart@%p  pci@%p  i2c@%p  hid_kb_addr=%u\n",
                (void *)uart_at, (void *)pci_at, (void *)i2c_at,
                (uint64_t)RVVM_I2C_HID_KEYBOARD);

    /* Bring up the speaker if RVVM was started with -hda_test. The CHIP-8
     * sound timer drives a single tone at ~444 Hz. */
    bool snd = hda_init();

    /* Bring up the framebuffer if RVVM was started with -bochs_display. */
    bool gfx = bochs_init(&bd, DISPLAY_W, DISPLAY_H);
    if (gfx) {
        bochs_fill(&bd, COLOR_BG);
        uart_puts("bochs: framebuffer up\n");
    } else {
        uart_puts("bochs: falling back to ANSI UART render\n");
    }

    chip8_reset(&vm, time_now());
    /* Enable the COSMAC display-wait quirk by default — most original
     * CHIP-8 games are written assuming DRW blocks until vblank, which
     * naturally clusters sprite operations to one per 60 Hz frame and
     * eliminates a class of sub-frame flicker. */
    chip8_set_vblank_wait(&vm, true);

    /* If RVVM was started with -ata <rom>.ch8, load the program off the
     * disk; otherwise fall back to the embedded IBM-logo splash. We
     * always attempt to read 7 sectors (=3.5 KiB, max CHIP-8 program
     * area). RVVM returns IDENTIFY capacity=0 for files smaller than
     * one sector, so don't gate on that — just try the read. */
    static uint8_t disk_buf[CHIP8_MEM_SIZE - CHIP8_PROGRAM_BASE];
    ata_t    disk;
    uint32_t got = 0;
    if (ata_init(&disk)) got = ata_read(&disk, 0, disk_buf, 7);
    if (got > 0) {
        uint64_t bytes = (uint64_t)got * 512;
        if (bytes > sizeof(disk_buf)) bytes = sizeof(disk_buf);
        chip8_load(&vm, disk_buf, bytes);
        uart_printf("loaded ROM from -ata: %u sectors (%u bytes)\n",
                    (uint64_t)got, bytes);
    } else {
        chip8_load(&vm, rom_ibm_logo, sizeof(rom_ibm_logo));
        uart_printf("loaded embedded IBM logo (%u bytes)\n",
                    (uint64_t)sizeof(rom_ibm_logo));
    }
    uart_puts("Keypad: 1234 / qwer / asdf / zxcv  (focus the GUI window)\n\n");

    uint64_t deadline = time_now() + RVVM_TICKS_PER_FRAME;
    bool     beeping  = false;       /* track ST-driven beep state */
    for (;;) {
        /* Drain HID events into the CHIP-8 key state. */
        hid_kb_poll(&kb, on_key_event, NULL);

        for (int i = 0; i < CYCLES_PER_FRAME && !vm.halted; i++) {
            chip8_step(&vm);
        }
        chip8_tick_60hz(&vm);

        /* CHIP-8 sound timer: tone while ST != 0. Switch the beeper
         * widget on/off only on transitions to avoid spamming verbs. */
        if (snd) {
            bool want = (vm.st != 0);
            if (want != beeping) {
                hda_beep(want ? RVVM_HDA_BEEP_DIV_440HZ : 0);
                beeping = want;
            }
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
