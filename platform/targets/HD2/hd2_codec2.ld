/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HD2 application linker script for the codec2 voice-prompt build.
 *
 * Identical to the board default (miosix arch/board/hrc7000_hd2/unikernel.ld +
 * ldscripts/miosix-flash-ram.ld) EXCEPT it RELOCATES the codec2 archive .text
 * (libcodec2_hd2.a) from flash to on-chip IRAM: the .codec2_ram output section
 * is linked for a RAM VMA (bottom of RAM, above the IRQ stack) with its load
 * image (LMA) in flash, and hd2_codec2_ram_init() copies it into place at boot.
 *
 * Why: the CK803S has a single 4 KB, 4-way, 16-byte-line I-cache; in FLASH the
 * codec2 decode hot loops are I-cache-set-collision-bound, so their per-frame
 * time depends on the absolute .text link address (a single function shift can
 * cost >1 ms) and -O2 backfires by reshuffling the set mapping.  IRAM execution
 * is uncached/collision-free, so a plain -O2 build runs fastest there with no
 * layout tuning (decode min ~12.4 ms/frame vs ~15.6 ms for -Os pinned in flash).
 *
 * The generic section layout below is inlined (not INCLUDEd) so .codec2_ram can
 * sit between .irq_stack and .data without editing the shared kernel
 * ldscripts/miosix-flash-ram.ld (which stays byte-identical to upstream; see
 * vendor/miosix-kernel/CODEC2_CACHE_PIN.prompt.md).  Keep this layout in sync
 * with that file if the kernel's copy ever changes.
 */

_irq_stack_size = 1024;

MEMORY
{
    flash(rx) : ORIGIN = 0x0300d000, LENGTH = 614400      /* IAP app slot, VMA==LMA */
    ram(wx)   : ORIGIN = 0x00020000, LENGTH = 0x2F000     /* 188 KiB: 0x20000..0x4f000 */
}

/* Default values for tunable variables */
PROVIDE(_irq_stack_size = 512);
PROVIDE(_process_pool_size = 0);
PROVIDE(_xram_start = 0); /* Used for memory protection and cacheability */
PROVIDE(_xram_size = 0);  /* Used for memory protection and cacheability */

/* Sanity checks */
ASSERT(_irq_stack_size % 8 == 0, "IRQ stack size error");
ASSERT(_process_pool_size % 1024 == 0, "Process pool size error");

ENTRY(_ZN6miosix13Reset_HandlerEv)

SECTIONS
{
    . = 0;

    .text :
    {
        /* Interrupt vectors must be placed at the start of flash */
        KEEP(*(.isr_vector))

        /* HD2: the codec2 archive .text lives in IRAM (.codec2_ram below),
         * copied there at boot by hd2_codec2_ram_init() -- IRAM execution is
         * uncached/collision-free, which removes the flash I-cache set-collision
         * penalty and lets the -O2 build run fastest (see the file header).
         * Exclude the archive from the general .text catch-alls here so its
         * input sections fall through to .codec2_ram. */
        . = ALIGN(4);
        *(EXCLUDE_FILE(*libcodec2_hd2.a) .text)
        *(EXCLUDE_FILE(*libcodec2_hd2.a) .text.*)
        *(.gnu.linkonce.t.*)
        /* these sections for thumb interwork? */
        *(.glue_7)
        *(.glue_7t)
        /* these sections for C++ */
        *(.gcc_except_table)
        *(.gcc_except_table.*)
        *(.ARM.extab*)
        *(.gnu.linkonce.armextab.*)

        . = ALIGN(4);
        /* .rodata: constant data */
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r.*)

        /* C++ Static constructors/destructors (eabi) */
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

        /* C++ Static constructors/destructors (elf)  */
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
    } > flash

    /* .ARM.exidx is sorted, so has to go in its own output section.  */
    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > flash
    __exidx_end = .;

    .irq_stack (NOLOAD) :
    {
        . = ORIGIN(ram);
        _irq_stack_bottom = .;
        . = . + _irq_stack_size;
        _irq_stack_top = .;
    } > ram

    /* HD2: codec2 code executed from IRAM.  Load image (LMA) in flash, copied to
     * this RAM VMA at boot by hd2_codec2_ram_init() before any codec2 call.  At
     * the BOTTOM of RAM (above the IRQ stack, below .data) -- a fixed low region
     * that neither the heap (grows up from .bss) nor the boot/thread stacks (top
     * of RAM / carved from the heap) can reach. */
    .codec2_ram : ALIGN(16)
    {
        __codec2_ram_start = .;
        *libcodec2_hd2.a:*(.text .text.*)
        . = ALIGN(8);
        __codec2_ram_end = .;
    } > ram AT > flash
    __codec2_ram_load = LOADADDR(.codec2_ram);

    .data : ALIGN(8)
    {
        _data = .;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        _edata = .;
    } > ram AT > flash
    _etext = LOADADDR(.data);

    _bss_start = .;
    .bss (NOLOAD) :
    {
        *(.bss)
        *(.bss.*)
        *(.gnu.linkonce.b.*)
        . = ALIGN(8);
    } > ram
    _bss_end = .;

    .heap (NOLOAD) :
    {
        _end = .;
        . = ORIGIN(ram) + LENGTH(ram) - _process_pool_size;
        _heap_end = .;
    } > ram

    .process_pool (NOLOAD) :
    {
        _process_pool_start = .;
        . = . + _process_pool_size;
        _process_pool_end = .;
    } > ram
}
