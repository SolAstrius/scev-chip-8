#include "uart.h"

void kmain(void) {
    uart_init();
    uart_puts("\n");
    uart_puts("scev-cores/chip-8 — bare-metal CHIP-8 on RVVM\n");
    uart_puts("hart 0 alive, sp set, bss zeroed.\n");
    uart_puts("CHIP-8 emulator: not yet wired up.\n");

    /* Halt forever. */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}
