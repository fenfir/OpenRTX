/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miosix-capable linker script for the Ailunce HD2 Phase-1 bring-up.
 * STAGING DRAFT -> platform/mcu/CSKY_V2/linker_script_miosix.ld.
 *
 * = the live-verified CSKY layout (linker_script.ld) PLUS the Miosix
 * init-array/ctor block (Ref C sec.2) so _init()/mainLoader can find
 * __miosix_init_array_*, __preinit_array_*, __init_array_*, _ctor_*.
 *
 * IRAM origin is 0x00020000 (NOT 0x10000): the Dahua IAP leaves live state
 * in 0x10000..0x18000 and zeroing .bss there kills UART2 (verified -- see
 * linker_script.ld comment / memory project_hd2_diagboot-c-works).  Stack
 * top stays at 0x00050000.
 */

ENTRY(_start)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x0300d000, LENGTH = 614400
    IRAM  (rwx): ORIGIN = 0x00020000, LENGTH = 0x30000   /* 192 KiB, top 0x50000 */
}

_main_stack_size = 0x00001000;                   /* 4 KiB; MUST == MAIN_STACK_SIZE */
_main_stack_top  = ORIGIN(IRAM) + LENGTH(IRAM);  /* = 0x00050000 */

SECTIONS
{
    .text : ALIGN(4) {
        KEEP(*(.text.startup))                  /* reset handler first */
        *(.text*)
        *(.rodata*)
        *(.gnu.linkonce.t.*)
        *(.gnu.linkonce.r.*)

        /* ---- Miosix C++ ctor/init-array block (Ref C sec.2) ---- *
         * Quoted from STM32F4xx linker_script_MDx.ld:100-144, with the
         * ARM-only .glue_7/.ARM.ex* lines dropped and > FLASH.            */
        . = ALIGN(4);
        KEEP(*(.init))

        . = ALIGN(4);
        __miosix_init_array_start = .;
        KEEP (*(SORT(.miosix_init_array.*)))
        KEEP (*(.miosix_init_array))
        __miosix_init_array_end = .;

        . = ALIGN(4);
        __preinit_array_start = .;
        KEEP (*(.preinit_array))
        __preinit_array_end = .;

        . = ALIGN(4);
        __init_array_start = .;
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;

        . = ALIGN(4);
        KEEP(*(.fini))

        . = ALIGN(4);
        __fini_array_start = .;
        KEEP (*(.fini_array))
        KEEP (*(SORT(.fini_array.*)))
        __fini_array_end = .;

        /* C++ static ctors/dtors (elf .ctors/.dtors) */
        . = ALIGN(4);
        _ctor_start = .;
        KEEP (*crtbegin.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*crtend.o(.ctors))
        _ctor_end = .;

        . = ALIGN(4);
        KEEP (*crtbegin.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*crtend.o(.dtors))

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
        end = .;                                /* newlib _sbrk */
        _end = .;                               /* Miosix _sbrk_r heap start (Ref C sec.2) */
    } > IRAM

    _heap_start = _ebss;
    _heap_end   = _main_stack_top - _main_stack_size;   /* = 0x4f000 (Ref C sec.2) */

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
