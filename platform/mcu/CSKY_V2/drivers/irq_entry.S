/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * !!! WORK IN PROGRESS -- DO NOT CONSUME !!!
 *
 * C-SKY V2 interrupt entry stub for the periodic-IRQ bring-up test
 * (irq_test.c).  GCC's __attribute__((interrupt)) is silently ignored by
 * our csky toolchain (it emits a normal `jmp r15` return with no context
 * save), so we hand-write the prologue/epilogue + `rte` here, mirroring
 * the vendor dispatcher (stm/ldm + rte).
 *
 * On a C-SKY V2 interrupt the core auto-saves PC->EPC and PSR->EPSR and
 * clears IE, then jumps to VBR[vector].  This stub is installed directly
 * as VBR[34] (Timer2).  It saves the GPRs the C body could clobber,
 * calls the plain C worker, restores, and `rte`s (which restores PC+PSR
 * from EPC/EPSR, re-enabling interrupts).  We do NOT re-enable interrupts
 * inside the handler, so EPC/EPSR need no manual save (non-nested).
 */

    .text
    .align  2
    .globl  timer2_isr_entry
    .type   timer2_isr_entry, @function
timer2_isr_entry:
    subi    sp, sp, 64
    stm     r0-r13, (sp)          /* save r0..r13 (covers all caller-saved) */
    st.w    r15, (sp, 56)         /* save r15 (link reg of interrupted code) */

    jbsr    timer2_isr_body       /* void timer2_isr_body(void) -- in irq_test.c */

    ld.w    r15, (sp, 56)
    ldm     r0-r13, (sp)
    addi    sp, sp, 64
    rte                           /* restore PC+PSR from EPC/EPSR */
    .size   timer2_isr_entry, .-timer2_isr_entry
