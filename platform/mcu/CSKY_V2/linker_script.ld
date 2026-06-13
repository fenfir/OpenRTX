/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Active HD2/CSKY V2 bring-up; layout is tentative.  See memory
 * project-hd2-openrtx-boots + project-hd2-v213-mmio-snapshot.
 *
 * Linker script for OpenRTX on the Ailunce HD2.
 *
 *   FLASH  0x0300d000  614400 bytes  -- the app slot the Dahua IAP
 *                                       loads us into.  Code, rodata,
 *                                       and .data initialisers all
 *                                       live here.
 *
 *   IRAM   0x00010000  256 KiB        -- on-chip SRAM.  Holds .data
 *                                       (writable copy), .bss, and the
 *                                       stack.  Top at 0x00050000.
 *
 * The vendor V2.1.3 firmware kept uC/OS-III kernel + per-task stacks in
 * this region too -- we don't need to be compatible with that, but the
 * size is well-attested.  See memory project-hd2-diagboot-c-works for
 * the live-verified layout.
 *
 * No exception table at a fixed VMA -- the CSKY V2 VBR register chooses
 * where vectors live, and we'll set that explicitly from platform_init()
 * once we need interrupts.  The IAP leaves the chip in IRQ-disabled
 * state so it's safe to deliberately not have one until then.
 */

ENTRY(_start)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x0300d000, LENGTH = 614400
    /*
     * IRAM starts at 0x00020000 not 0x00010000 -- the Dahua IAP appears
     * to leave live state in the 0x10000..0x18000 region (vendor V2.1.3
     * RTOS lives there too, and a 40 KiB .bss-zero pass that overlapped
     * killed UART2 silently right after the IAP "Execute The Program"
     * jump; without the .bss touching that range the no-display build
     * worked).  192 KiB starting at 0x20000 covers our .data/.bss/heap/
     * stack with room to spare.  Stack top stays at 0x50000.
     */
    IRAM  (rwx): ORIGIN = 0x00020000, LENGTH = 0x30000
}

_main_stack_size = 0x00001000;                   /* 4 KiB main stack */
_main_stack_top  = ORIGIN(IRAM) + LENGTH(IRAM);  /* = 0x00050000 */

SECTIONS
{
    .text : ALIGN(4) {
        KEEP(*(.text.startup))                  /* reset handler first */
        *(.text*)
        *(.rodata*)
        *(.gnu.linkonce.t.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4);
        _sidata = .;                            /* LMA where .data init lives */
    } > FLASH

    .data : AT(_sidata) ALIGN(4) {
        _sdata = .;
        *(.data*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4);
        _edata = .;
    } > IRAM

    .bss (NOLOAD) : ALIGN(4) {
        _sbss = .;
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        end = .;                                /* needed by newlib _sbrk */
        _end = .;
    } > IRAM

    /* Track heap start for newlib _sbrk implementation in stubs. */
    _heap_start = _ebss;
    _heap_end   = _main_stack_top - _main_stack_size;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.note.*)
        *(.comment)
        *(.ARM.attributes)
        *(.csky.attributes)
        *(.debug*)
    }
}
