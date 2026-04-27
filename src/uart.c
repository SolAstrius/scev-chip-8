#include "uart.h"
#include "mmio.h"
#include "rvvm.h"

#define UART_BASE   RVVM_UART_BASE

/* Register offsets — match src/devices/ns16550a.c lines 35-47. */
#define UART_RBR    0x0   /* RX buffer        (read,  DLAB=0) */
#define UART_THR    0x0   /* TX hold          (write, DLAB=0) */
#define UART_DLL    0x0   /* divisor low      (R/W,   DLAB=1) */
#define UART_IER    0x1   /* irq enable       (R/W,   DLAB=0) */
#define UART_DLM    0x1   /* divisor high     (R/W,   DLAB=1) */
#define UART_IIR    0x2   /* irq ident        (read) */
#define UART_FCR    0x2   /* FIFO control     (write) */
#define UART_LCR    0x3   /* line control */
#define UART_MCR    0x4   /* modem control */
#define UART_LSR    0x5   /* line status */
#define UART_MSR    0x6   /* modem status */
#define UART_SCR    0x7   /* scratch */

/* LSR bits (16550A spec). */
#define LSR_DR      0x01  /* data ready (RX) */
#define LSR_THRE    0x20  /* TX hold register empty */
#define LSR_TEMT    0x40  /* TX shift register empty too */

/* LCR bits. */
#define LCR_8N1     0x03  /* 8 data bits, no parity, 1 stop */
#define LCR_DLAB    0x80  /* divisor latch access */

/* FCR bits. */
#define FCR_ENABLE  0x01  /* enable FIFOs */
#define FCR_CLR_RX  0x02  /* clear RX FIFO */
#define FCR_CLR_TX  0x04  /* clear TX FIFO */
#define FCR_TRIG14  0xC0  /* RX FIFO trigger at 14 bytes */

/* MCR bits. */
#define MCR_DTR     0x01
#define MCR_RTS     0x02
#define MCR_OUT2    0x08  /* must be set on real PC hw to enable IRQs out */

static inline uint8_t r(uint32_t off) { return mmio_r8(UART_BASE + off); }
static inline void    w(uint32_t off, uint8_t v) { mmio_w8(UART_BASE + off, v); }

void uart_init(void) {
    /* Real 16550A init sequence. RVVM ignores most of it but doing it
     * properly costs nothing and keeps the driver portable. */
    w(UART_IER, 0x00);              /* mask all UART interrupts */
    w(UART_LCR, LCR_DLAB);          /* expose divisor latch */
    w(UART_DLL, 0x01);              /* divisor = 1 → ~max speed; RVVM ignores */
    w(UART_DLM, 0x00);
    w(UART_LCR, LCR_8N1);           /* 8N1, divisor latch closed */
    w(UART_FCR, FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX | FCR_TRIG14);
    w(UART_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);
}

void uart_putc(char c) {
    while (!(r(UART_LSR) & LSR_THRE)) { /* busy-wait */ }
    w(UART_THR, (uint8_t)c);
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_write(const void *buf, uint64_t len) {
    const char *p = buf;
    for (uint64_t i = 0; i < len; i++) uart_putc(p[i]);
}

int uart_getc_nb(void) {
    if (r(UART_LSR) & LSR_DR) {
        return (int)(uint8_t)r(UART_RBR);
    }
    return -1;
}

char uart_getc(void) {
    int c;
    while ((c = uart_getc_nb()) < 0) { /* busy-wait */ }
    return (char)c;
}

/* --- Number formatting. ----------------------------------------------- */

static const char hex_digits[] = "0123456789abcdef";

void uart_put_hex8(uint8_t v) {
    uart_putc(hex_digits[(v >> 4) & 0xF]);
    uart_putc(hex_digits[v & 0xF]);
}

void uart_put_hex32(uint32_t v) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) uart_putc(hex_digits[(v >> i) & 0xF]);
}

void uart_put_hex64(uint64_t v) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc(hex_digits[(v >> i) & 0xF]);
}

void uart_put_udec(uint64_t v) {
    char buf[24];
    int  i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

void uart_put_dec(int64_t v) {
    if (v < 0) { uart_putc('-'); v = -v; }
    uart_put_udec((uint64_t)v);
}

void uart_hexdump(const void *buf, uint64_t len) {
    const uint8_t *p = buf;
    for (uint64_t off = 0; off < len; off += 16) {
        uart_put_hex64((uint64_t)(uintptr_t)(p + off));
        uart_puts("  ");
        /* hex columns */
        for (uint64_t i = 0; i < 16; i++) {
            if (off + i < len) { uart_put_hex8(p[off + i]); uart_putc(' '); }
            else                 uart_puts("   ");
            if (i == 7) uart_putc(' ');
        }
        uart_puts(" |");
        /* ASCII gutter */
        for (uint64_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = p[off + i];
            uart_putc((c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        uart_puts("|\n");
    }
}

/* --- Tiny printf. ----------------------------------------------------- */

void uart_vprintf(const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { uart_putc(*fmt++); continue; }
        fmt++;
        switch (*fmt++) {
        case 'c': uart_putc((char)va_arg(ap, int));               break;
        case 's': uart_puts(va_arg(ap, const char *));            break;
        case 'd': uart_put_dec(va_arg(ap, int64_t));              break;
        case 'u': uart_put_udec(va_arg(ap, uint64_t));            break;
        case 'x': uart_put_hex64(va_arg(ap, uint64_t));           break;
        case 'p': uart_put_hex64((uint64_t)va_arg(ap, void *));   break;
        case '%': uart_putc('%');                                 break;
        case 0:   return;   /* trailing % */
        default:  uart_putc('?');                                 break;
        }
    }
}

void uart_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
}
