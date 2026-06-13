/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miosix crt0 for the Ailunce HD2 (CK803S).  STAGING DRAFT ->
 * platform/mcu/CSKY_V2/boot/startup_miosix.S.
 *
 * = the live-verified startup.S, with the final `bsr main` replaced by
 * `bsr _init` (Ref C sec.1: crt0 must hand off to the single Miosix entry
 * symbol _init(), NOT call main directly).  Interrupts stay DISABLED (the
 * IAP hands off IRQ-off; _init asserts INTERRUPTS_ENABLED_AT_BOOT otherwise).
 */

    .text
    .align  2
    .section .text.startup, "ax"
    .globl  _start
    .globl  Reset_Handler
Reset_Handler:
_start:
    /* snapshot IAP-left r28 (GBR) into the fixed slot (kept from startup.S) */
    lrw     r0, 0x0004fff8
    st.w    r28, (r0, 0)

    /* 1. set SP = top of IRAM */
    lrw     r0, _main_stack_top
    mov     r14, r0

    /* 2. copy .data FLASH LMA -> IRAM VMA */
    lrw     r5, _sdata
    lrw     r6, _edata
    sub     r6, r5
    bez     r6, 2f
    lrw     r4, _sidata
1:  ld.w    r0, (r4, 0)
    st.w    r0, (r5, 0)
    addi    r4, r4, 4
    addi    r5, r5, 4
    subi    r6, r6, 4
    bnez    r6, 1b
2:

    /* 3. zero .bss */
    lrw     r5, _sbss
    lrw     r6, _ebss
    sub     r6, r5
    bez     r6, 4f
    movi    r0, 0
3:  st.w    r0, (r5, 0)
    addi    r5, r5, 4
    subi    r6, r6, 4
    bnez    r6, 3b
4:

    /* 4. hand off to Miosix _init() (NOT main).  Never returns; halt if it does. */
    bsr     _init
5:  br      5b
