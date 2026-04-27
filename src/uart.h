/* Tiny NS16550A driver — just enough for print debugging.
 * RVVM places ns16550a at 0x10000000 by default. */

#pragma once
#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex(uint64_t val);
