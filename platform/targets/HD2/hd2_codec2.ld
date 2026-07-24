/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HD2 application linker script for the codec2 voice-prompt build.
 *
 * Identical to the board default (miosix arch/board/hrc7000_hd2/unikernel.ld +
 * ldscripts/miosix-flash-ram.ld) EXCEPT it pins the codec2 decode code
 * (libcodec2_hd2.a .text) to a 4 KB-aligned address at the very top of .text.
 * The CK803S has a single 4 KB, 4-way, 16-byte-line I-cache (64 sets, set index
 * = addr bits [9:4]); the codec2 decode hot loops are I-cache-collision-bound,
 * and their per-frame time depends on the absolute link address of the codec2
 * .text -- unrelated firmware-size changes elsewhere reshuffle the set mapping
 * and swing decode by ~3 ms.  Pinning it to a fixed 4 KB boundary makes the
 * set mapping deterministic and, with an -Os codec build (~3.9 KB working set),
 * keeps the hot path resident in the I-cache.
 *
 * The generic section layout below is inlined (not INCLUDEd) so the pin can sit
 * inside .text without editing the shared kernel ldscripts/miosix-flash-ram.ld,
 * which must stay byte-identical to upstream (see
 * vendor/miosix-kernel/CODEC2_CACHE_PIN.prompt.md).  Keep this section layout in
 * sync with that file if the kernel's copy ever changes.
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

        /* HD2: pin the codec2 archive .text to a 4 KB (I-cache-size) boundary up
         * front, so its cache-set mapping is deterministic and immune to the
         * rest of the firmware's layout.  The decode hot loops are
         * I-cache-set-collision-sensitive -- their per-frame time depends on the
         * absolute link address of the codec2 .text (a single function shift can
         * cost >1 ms), so pinning the whole archive to a fixed 4 KB boundary
         * keeps the set mapping deterministic.  The archive MUST stay monolithic
         * (built -Os, NO -ffunction-sections) so the hand-tuned intra-object
         * layout (lpc.o byte-identical, aks_to_mag2 off a colliding set) holds. */
        . = ALIGN(4096);
        __codec2_text_start = .;
        *libcodec2_hd2.a:*(.text .text.*)
        __codec2_text_end = .;
        . = ALIGN(4);

        *(.text)
        *(.text.*)
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
