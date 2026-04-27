#include "uart.h"

extern char __bss_start[], __bss_end[];
extern char __stack_top[];

void kmain(void) {
    uart_init();
    uart_puts("\n");
    uart_puts("scev-cores/chip-8 — bare-metal CHIP-8 on RVVM\n");
    uart_puts("─────────────────────────────────────────────\n");

    uart_printf("hart 0 alive.  sp=%p  bss=%p..%p (%u bytes zeroed)\n",
                __stack_top, __bss_start, __bss_end,
                (uint64_t)(__bss_end - __bss_start));

    /* Exercise the formatted output so we know it works. */
    uart_printf("printf check: dec=%d  udec=%u  hex=%x  char=%c  str=%s\n",
                (int64_t)-42, (uint64_t)1234567, (uint64_t)0xDEADBEEFCAFEBABEULL,
                (int)'!', "ok");

    /* Hex-dump the start of our own .text so we can see real memory. */
    extern void _start(void);
    uart_puts("\nfirst 64 bytes of .text:\n");
    uart_hexdump((const void *)_start, 64);

    uart_puts("\nUART RX echo: type a char, see it back. Ctrl-A x to quit RVVM.\n");
    for (;;) {
        int c = uart_getc_nb();
        if (c >= 0) {
            uart_printf("[rx %x] ", (uint64_t)(uint8_t)c);
            uart_putc((char)c);
            if (c == '\r') uart_putc('\n');
        }
        /* Spin between polls. wfi would be nicer but we'd need timer IRQ
         * to wake; for now just burn cycles. */
        for (volatile int i = 0; i < 1000; i++) {}
    }
}
