/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miosix arch_registers_impl.h for the CK803S (C-SKY V2) core.
 * STAGING DRAFT -> lib/miosix-kernel/miosix/arch/cskyv2/arch_registers_impl.h.
 *
 * The cortexM0 version pulls in CMSIS device headers (Ref A's
 * arch_registers_impl.h).  CK803S has no CMSIS; instead we expose the raw
 * MMIO base addresses and the inline control-register (PSR/VBR/EPC/EPSR)
 * accessors the port needs.  All addresses are from Reference B (verified
 * in-tree via irq_test.c / irq_entry.S / docs/irq_handlers.md).
 *
 * NO MAGIC NUMBERS rule (memory feedback_no_magic_numbers): every MMIO
 * address used by the port is a named macro defined here.
 */

#ifndef ARCH_REGISTERS_IMPL_H
#define ARCH_REGISTERS_IMPL_H

#include <stdint.h>

/* ---- DW_apb_timers @ 0x14000000, channel stride 0x14 (Ref B sec.6) ----
 * ch0/Timer1 = free-running getTick timebase (delays.c) -- DO NOT touch.
 * ch1/Timer2 = the Miosix preemption tick.                              */
#define HD2_TIMER_BASE        0x14000000u
#define HD2_TMR(ch, off)      (*(volatile uint32_t *)(HD2_TIMER_BASE + (ch) * 0x14u + (off)))
#define HD2_T2_LOAD           HD2_TMR(1u, 0x00u)   /* reload value          */
#define HD2_T2_CTRL           HD2_TMR(1u, 0x08u)   /* b0 en b1 reload b2 imask */
#define HD2_T2_EOI            HD2_TMR(1u, 0x0cu)   /* READ clears Timer2 IRQ */

/* ---- PIC interrupt controller @ 0x17000000 (Ref B sec.6) ------------- */
#define HD2_PIC_BASE          0x17000000u
#define HD2_PIC_MODE          (*(volatile uint32_t *)(HD2_PIC_BASE + 0x00u))
#define HD2_PIC_PO            (*(volatile uint32_t *)(HD2_PIC_BASE + 0x04u))
#define HD2_PIC_MASK          (*(volatile uint32_t *)(HD2_PIC_BASE + 0x08u)) /* bit=0 ENABLES */
#define HD2_PIC_COW1          (*(volatile uint32_t *)(HD2_PIC_BASE + 0x10u)) /* int-end: write 4 */
#define HD2_PIC_INT_ST        (*(volatile uint32_t *)(HD2_PIC_BASE + 0x44u))
#define HD2_PIC_INT_ST1       (*(volatile uint32_t *)(HD2_PIC_BASE + 0x48u))
#define HD2_PIC_MODE1         (*(volatile uint32_t *)(HD2_PIC_BASE + 0x60u))
#define HD2_PIC_PO1           (*(volatile uint32_t *)(HD2_PIC_BASE + 0x64u))

/* Timer2 = PIC source 2 -> autovector 32+2 = 34 (Ref B sec.4/6). */
#define HD2_TIMER2_SRC        2u
#define HD2_TIMER2_VEC        34u

/* Timer input clock: 42 MHz, measured (memory project_hd2_timebase). */
#define HD2_TIMER_HZ          42000000u

/* trap #n that doYield uses -> low-vector region.  trap0 = vector 32+0? NO:
 * traps land in the CPU-exception low-vector region 0x00..0x1f, NOT the
 * 0x20+ PIC range.  CK803S maps "trap #n" to a fixed exception vector.
 * We reserve vector index 16 (0x10) for the yield trap (see cskyv2_vectors.S
 * and OPEN QUESTIONS in BUILD_NOTES.md -- this index must be HW-confirmed). */
#define HD2_YIELD_TRAP_NO     0u
#define HD2_YIELD_VEC         16u   /* VBR[16] = yield entry stub (tentative) */

/* PSR bit positions (Ref B sec.3, standard CK803S layout -- verify on HW). */
#define HD2_PSR_IE_BIT        6u    /* maskable-interrupt enable */
#define HD2_PSR_EE_BIT        8u    /* exception enable          */

/* BOOTROM reset entry for IRQsystemReboot (memory project_hd2_bootrom_reset):
 * jump to 0x00000004, NOT stage-1 0x03000000. */
#define HD2_BOOTROM_RESET     0x00000004u

/* ---------- inline control-register accessors (Ref B sec.3/5) ---------- */

static inline unsigned int csky_get_psr(void)
{
    unsigned int v;
    asm volatile("mfcr %0, psr" : "=r"(v));
    return v;
}

static inline void csky_set_psr(unsigned int v)
{
    asm volatile("mtcr %0, psr" :: "r"(v));
}

static inline void csky_set_vbr(const void *table)
{
    /* mtcr rN, cr<1,0>  (VBR = cr1) -- irq_test.c line 101 form. */
    asm volatile("mtcr %0, cr<1, 0>" :: "r"(table));
}

#endif //ARCH_REGISTERS_IMPL_H
