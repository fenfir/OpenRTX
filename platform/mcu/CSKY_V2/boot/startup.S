/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Active HD2/CSKY V2 bring-up.  Other agents: do not refactor or
 * extend this code as a settled implementation.  See memory
 * project-hd2-openrtx-* for context.
 *
 * Reset handler for OpenRTX on Ailunce HD2 (CSKY V2 ck803s MCU,
 * Dahua HR_C7000 SoC).
 *
 * Loaded by the Dahua IAP at app entry VMA 0x0300d000.  Sets up the
 * minimum C runtime so main() can run as ordinary code:
 *
 *   1. SP <- _main_stack_top (top of IRAM, 0x00050000).
 *   2. Copy .data initialiser image from FLASH LMA to IRAM VMA.
 *   3. Zero .bss.
 *   4. Call main.  Halt on return.
 *
 * No interrupts enabled, no FPU (ck803s has none), no VBR setup.  The
 * IAP leaves interrupts disabled when it hands off.  When OpenRTX wants
 * interrupts it will set them up explicitly in platform_init().
 *
 * Symbols come from linker_script.ld:
 *   _main_stack_top  end of stack region in IRAM (highest address)
 *   _sidata          LMA of .data in FLASH (start of init image)
 *   _sdata,_edata    VMA range of .data in IRAM
 *   _sbss,_ebss      VMA range of .bss in IRAM
 *
 * Mirrors the verified-working src/diagboot-c/crt0.S in spirit -- if
 * that booted, this should too.  See memory project-hd2-diagboot-c-works.
 */

    .text
    .align  2
    .section .text.startup, "ax"
    .globl  _start
    .globl  Reset_Handler          /* alias kept for ARM-style entry names */
Reset_Handler:
_start:
    /* ---- 0. snapshot IAP-left registers BEFORE anything else ---- *
     * Saves r28 (gp / gb under CSKY ABI) into a fixed slot at the
     * top of IRAM where C code can read it back via the dynamic
     * loader's `G` command.  Used to hunt the actual MMIO base of
     * GBR-relative globals like g_lcd_bus_mmio_latch.            */
    lrw     r0, 0x0004fff8                  /* slot under main stack */
    st.w    r28, (r0, 0)

    /* ---- 1. set SP ---- */
    lrw     r0, _main_stack_top
    mov     r14, r0

    /* ---- 2. copy .data from FLASH LMA to IRAM VMA ---- */
    lrw     r5, _sdata
    lrw     r6, _edata
    sub     r6, r5                  /* r6 = byte count */
    bez     r6, 2f
    lrw     r4, _sidata
1:  ld.w    r0, (r4, 0)
    st.w    r0, (r5, 0)
    addi    r4, r4, 4
    addi    r5, r5, 4
    subi    r6, r6, 4
    bnez    r6, 1b
2:

    /* ---- 3. zero .bss ---- */
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

    /* ---- 4. call main; halt on return ---- */
    bsr     main
5:  br      5b
