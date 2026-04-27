/* Entry point. mtd-physmap drops us at 0x80000000 with no stack, no zero'd
 * bss, no nothing. Set sp, zero bss, jump to kmain. */

    .section .text.boot, "ax"
    .global _start
    .extern __bss_start
    .extern __bss_end
    .extern __stack_top
    .extern kmain

_start:
    /* Hart 0 only — park any other harts in a wfi loop. */
    csrr    t0, mhartid
    bnez    t0, park

    /* Set up stack — top of the reserved region in link.ld. */
    la      sp, __stack_top

    /* Zero BSS. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:

    /* Off we go. */
    call    kmain

    /* If kmain returns, halt. */
park:
    wfi
    j       park
