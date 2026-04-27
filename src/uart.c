#include "uart.h"

/* RVVM ns16550a default base. See src/devices/ns16550a.h in RVVM. */
#define UART_BASE   0x10000000UL
#define UART_THR    0x0   /* Transmit Holding Register */
#define UART_LSR    0x5   /* Line Status Register */
#define UART_LSR_THRE 0x20 /* THR empty */

static inline volatile uint8_t *uart_reg(uint32_t off) {
    return (volatile uint8_t *)(UART_BASE + off);
}

void uart_init(void) {
    /* RVVM's ns16550a doesn't need real init — DLL/DLM are stored but ignored,
     * MCR/MSR are inert. Leave defaults. */
}

void uart_putc(char c) {
    while (!(*uart_reg(UART_LSR) & UART_LSR_THRE)) {}
    *uart_reg(UART_THR) = (uint8_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_put_hex(uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0xF]);
    }
}
