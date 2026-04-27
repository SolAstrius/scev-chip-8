#include "uart.h"
#include "time.h"
#include "chip8.h"
#include "gfx.h"
#include "pci.h"
#include "fdt.h"
#include "i2c.h"
#include "hid.h"
#include "ata.h"
#include "hda.h"
#include "roms.h"
#include <stddef.h>

/* "scev fat-ROM" container — optional 16-byte header, magic 'SCEVCH8'
 * + 1-byte version (1 or 2), then per-ROM config carried with the file.
 *
 * Version 1 (legacy):
 *   off 0..7   magic + version=1
 *   off 8      cycles_per_frame (u8,  0 = default)
 *   off 9      flags (bit 0: vblank-wait quirk)
 *   off 10..15 reserved
 *
 * Version 2 (current — wider tickrate for XO-CHIP-style speeds):
 *   off 0..7   magic + version=2
 *   off 8..9   cycles_per_frame (u16 LE, 0 = default)
 *   off 10     flags (bit 0: vblank-wait quirk)
 *   off 11..15 reserved
 *
 * Backward compat: v1 layout had byte 9 as flags and bytes 10..15 as
 * reserved/zero. v2 reads bytes 8..9 as a u16 LE; an old v1 file with
 * tickrate=N in byte 8 reads in v2 as tickrate=N (since byte 9 was
 * always 0 — flags bit 0 only). Old v1 files DO have flags in byte 9
 * though, so reading them under v2 rules would conflate flags into
 * the tickrate high byte. We dispatch on version explicitly to be safe.
 *
 * Headerless files (no magic) are loaded as raw CHIP-8 with defaults. */
#define ROM_MAGIC0       'S'
#define ROM_MAGIC1       'C'
#define ROM_MAGIC2       'E'
#define ROM_MAGIC3       'V'
#define ROM_MAGIC4       'C'
#define ROM_MAGIC5       'H'
#define ROM_MAGIC6       '8'
#define ROM_HEADER_LEN   16
#define ROM_FLAG_VBLANK  0x01

static bool rom_header_magic_ok(const uint8_t *p) {
    return p[0] == ROM_MAGIC0 && p[1] == ROM_MAGIC1 && p[2] == ROM_MAGIC2
        && p[3] == ROM_MAGIC3 && p[4] == ROM_MAGIC4 && p[5] == ROM_MAGIC5
        && p[6] == ROM_MAGIC6;
}

extern char __bss_start[], __bss_end[];

/* Default cycles/frame when no fat-ROM header overrides it. Octo's
 * chip8Archive medians around 15-30 here. Tank wants 200 — pack a
 * fat-ROM header on it via mkrom.py --tickrate 200. */
#define CYCLES_PER_FRAME_DEFAULT  30
#define CHIP8_SCALE         10
#define DISPLAY_W           (CHIP8_DISPLAY_W * CHIP8_SCALE)
#define DISPLAY_H           (CHIP8_DISPLAY_H * CHIP8_SCALE)

#define COLOR_FG            0x00B0FFB0U
#define COLOR_BG            0x00101010U

static chip8_t        vm;
static gfx_t          g;
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

    /* Arrow keys as a convenience alias for the directional cluster.
     * Most CHIP-8 games use the WASD-shape: 5=up, 8=down, 4=left, 6=right
     * (the central column of the hex pad). */
    case 0x52: return 0x5;     /* Up arrow */
    case 0x51: return 0x8;     /* Down arrow */
    case 0x50: return 0x4;     /* Left arrow */
    case 0x4F: return 0x6;     /* Right arrow */

    default:   return 0xFF;
    }
}

static void on_key_event(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    uint8_t k = hid_to_chip8(usage);
    /* Trace EVERY key, mapped or not. If you press 1234/qwer/asdf/zxcv
     * and don't see this log line, the i2c-hid path is broken; if you
     * see the log but nothing happens in the game, the game probably
     * doesn't use that key (caveexplorer = Q/E/A/D, not 1234). */
    /* Note: uart's %x already prepends "0x", so formats only carry text. */
    uart_printf("hid: usage=%x %s -> chip8 key %s%x\n",
                (uint64_t)usage, pressed ? "DOWN" : "up  ",
                k == 0xFF ? "(unmapped) " : "", (uint64_t)k);
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

    /* Bring up a framebuffer — bochs (mode-set to DISPLAY_W×DISPLAY_H)
     * or simple-framebuffer (whatever -res RVVM was started with).
     * Falls back to ANSI UART rendering if neither is present. */
    bool have_gfx = gfx_init_fdt(&g, &fdt, DISPLAY_W, DISPLAY_H);
    if (have_gfx) {
        gfx_fill(&g, COLOR_BG);
        uart_puts("gfx: framebuffer up\n");
    } else {
        uart_puts("gfx: falling back to ANSI UART render\n");
    }

    chip8_reset(&vm, time_now());

    /* Per-ROM config — defaults overridden by the fat-ROM header below. */
    uint32_t cycles_per_frame = CYCLES_PER_FRAME_DEFAULT;
    bool     vblank_wait      = false;

    /* If RVVM was started with -ata <rom>.ch8, load the program off the
     * disk; otherwise fall back to the embedded IBM-logo splash. */
    static uint8_t disk_buf[CHIP8_MEM_SIZE - CHIP8_PROGRAM_BASE];
    ata_t    disk;
    uint32_t got = 0;
    if (ata_init(&disk)) got = ata_read(&disk, 0, disk_buf, 7);
    if (got > 0) {
        uint64_t bytes = (uint64_t)got * 512;
        const uint8_t *prog = disk_buf;

        /* Fat-ROM header? Strip it and apply per-ROM config. */
        if (bytes >= ROM_HEADER_LEN && rom_header_magic_ok(disk_buf)) {
            uint8_t  ver       = disk_buf[7];
            uint16_t hdr_tick  = 0;
            uint8_t  hdr_flags = 0;
            if (ver == 1) {
                hdr_tick  = disk_buf[8];
                hdr_flags = disk_buf[9];
            } else if (ver == 2) {
                hdr_tick  = disk_buf[8] | ((uint16_t)disk_buf[9] << 8);
                hdr_flags = disk_buf[10];
            } else {
                uart_printf("ROM hdr: unknown version %u, ignoring\n",
                            (uint64_t)ver);
                goto no_header;
            }
            if (hdr_tick) cycles_per_frame = hdr_tick;
            if (hdr_flags & ROM_FLAG_VBLANK) vblank_wait = true;
            prog   = disk_buf + ROM_HEADER_LEN;
            bytes -= ROM_HEADER_LEN;
            uart_printf("ROM hdr v%u: tickrate=%u  vblank_wait=%u\n",
                        (uint64_t)ver, (uint64_t)cycles_per_frame,
                        (uint64_t)vblank_wait);
        }
no_header:

        if (bytes > sizeof(disk_buf) - ROM_HEADER_LEN) {
            bytes = sizeof(disk_buf) - ROM_HEADER_LEN;
        }
        chip8_load(&vm, prog, bytes);
        uart_printf("loaded ROM from -ata: %u sectors (%u prog bytes)\n",
                    (uint64_t)got, bytes);
    } else {
        chip8_load(&vm, rom_ibm_logo, sizeof(rom_ibm_logo));
        uart_printf("loaded embedded IBM logo (%u bytes)\n",
                    (uint64_t)sizeof(rom_ibm_logo));
    }

    chip8_set_vblank_wait(&vm, vblank_wait);
    uart_puts("Keypad: 1234 / qwer / asdf / zxcv  (focus the GUI window)\n\n");

    uint64_t deadline = time_now() + RVVM_TICKS_PER_FRAME;
    bool     beeping  = false;       /* track ST-driven beep state */
    for (;;) {
        /* Drain HID events into the CHIP-8 key state. */
        hid_kb_poll(&kb, on_key_event, NULL);

        for (uint32_t i = 0; i < cycles_per_frame && !vm.halted; i++) {
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
            if (have_gfx) {
                /* Centre on the surface. Bochs mode-sets to exactly
                 * DISPLAY_W×DISPLAY_H so x_off/y_off both end up 0;
                 * simple-framebuffer is whatever -res RVVM was given,
                 * which is usually larger. */
                uint32_t x_off = (g.width  > DISPLAY_W) ? (g.width  - DISPLAY_W) / 2 : 0;
                uint32_t y_off = (g.height > DISPLAY_H) ? (g.height - DISPLAY_H) / 2 : 0;
                chip8_render_gfx(&vm, &g, CHIP8_SCALE, x_off, y_off,
                                 COLOR_FG, COLOR_BG);
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
