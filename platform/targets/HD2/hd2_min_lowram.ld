/*
 * Low-RAM-origin linker script for the Ailunce HD2 min-bringup.
 *
 * Identical to the board's hrc7000_hd2/unikernel.ld EXCEPT the RAM region starts
 * at 0x20000 instead of 0x33000. The stock board script reserves 0x1bfe0..0x327e8
 * (~76 KiB) for the AMBE+2 DMR-voice codec blob -- a hand-carved, non-relocatable
 * image that must run at its native VMA. The min-bringup is radio-free (no DMR,
 * no AMBE, and per project decision won't implement it), so that window is dead
 * space here. Reclaiming it grows the app RAM from 112 KiB -> 188 KiB, which the
 * codec2 voice-prompt state needs (~29 KiB static codec2_t + a healthy heap for
 * the Miosix thread stacks; the stock 112 KiB heap-starved the boot).
 *
 * Below 0x20000 is the IAP/BOOTROM low IRAM (VBR + early boot) -- left untouched.
 * The top 4 KiB (0x4f000..0x50000) stays excluded for the hard-lock trace block
 * (survives a WDT reset, below the region the IAP scribbles each boot).
 *
 * The generic Miosix script (included below) defines all sections/symbols
 * (_etext, _data, _bss_start/_end, ctors, _heap_end, _irq_stack_top, ENTRY).
 * This script only overrides MEMORY + the IRQ-stack tunable.
 */

_irq_stack_size = 1024;

MEMORY
{
    flash(rx) : ORIGIN = 0x0300d000, LENGTH = 614400
    ram(wx)   : ORIGIN = 0x00020000, LENGTH = 0x2F000   /* 188 KiB: 0x20000..0x4f000 */
}

INCLUDE ldscripts/miosix-flash-ram.ld
