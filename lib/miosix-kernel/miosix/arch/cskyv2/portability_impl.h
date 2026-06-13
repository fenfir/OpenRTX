/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miosix portability_impl.h for the CK803S (C-SKY V2) core.
 * STAGING DRAFT -> lib/miosix-kernel/miosix/arch/cskyv2/portability_impl.h.
 *
 * Provides: the global `ctxsave` pointer, the saveContext()/restoreContext()
 * macros, and the four inline interrupt-control functions.  Conforms to the
 * cortexM0 contract (Ref A sec.1 + sec.6) but uses CK803S mechanics (Ref B).
 *
 * ============================ DESIGN NOTE ==============================
 * CK803S has NO HW register auto-stacking (Ref B sec.5).  Our context-switch
 * model therefore differs from ARM in WHERE registers live, but satisfies
 * the same contract (ctxsave[0] == thread SP; restore lands in threadLauncher):
 *
 *   1. The naked vector entry stub (cskyv2_vectors.S) runs FIRST on any
 *      trap/Timer2 IRQ.  It pushes the FULL register frame (volatile +
 *      callee-saved + LR + PSR, 80 bytes = CTXSAVE_ON_STACK) onto the
 *      *interrupted thread's* stack, then writes the post-push SP into
 *      *ctxsave (i.e. cur->ctxsave[0]).  THAT stub's stm is the real
 *      "saveContext".
 *   2. saveContext() / restoreContext() below are the C++-macro hooks the
 *      contract names, but because the stub already did the heavy save,
 *      here they only need to (save) re-publish SP into *ctxsave and
 *      (restore) reload SP from *ctxsave.  They are written so that calling
 *      saveContext() at the top and restoreContext() at the bottom of the
 *      stub's C-callable body is a no-op-safe re-sync -- see cskyv2_vectors.S
 *      which does the canonical stm/ldm and calls the ISR body in between.
 *
 * For Phase-1 the entire save/restore is done in the asm stub (the proven
 * irq_entry.S pattern, Ref B sec.2).  These macros are provided for contract
 * completeness and for any future pure-asm naked handler that wants the
 * cortexM0-style "saveContext(); bl ISR; restoreContext();" shape.
 * =======================================================================
 */

#ifndef PORTABILITY_IMPL_H
#define PORTABILITY_IMPL_H

/*
 * Pull in the CK803S register macros + control-reg accessors.  NOTE: the
 * kernel's interfaces/arch_registers.h hardcodes #include
 * "boot/arch_registers_impl.h" -- the Cortex boards satisfy that via a
 * board-level boot/arch_registers_impl.h on the include path.  For the
 * standalone Phase-1 test we include the arch impl directly so the macros
 * resolve regardless; the final tree should ALSO drop a one-line
 * platform/mcu/CSKY_V2/boot/arch_registers_impl.h that #includes this arch
 * header, so interfaces/arch_registers.h works for the folded-in hd2 target.
 */
#include "arch_registers_impl.h"
#include "config/miosix_settings.h"

/*
 * C-linkage pointer to the current thread's ctxsave[] array.  The kernel
 * (IRQfindNextThread) repoints this at cur->ctxsave on every switch; the
 * arch reads/writes *ctxsave (== ctxsave[0] == thread SP).  Ref A sec.1.
 */
extern "C" {
extern volatile unsigned int *ctxsave;
}

/**
 * \internal
 * saveContext(): publish the current (already-on-stack) thread SP into
 * *ctxsave.  On CK803S the entry stub has already pushed the register frame
 * and left SP pointing at it; we just record SP into ctxsave[0].  The
 * "sync" memory barrier replaces ARM's dmb (Ref A sec.1: a barrier here is
 * mandatory to avoid the pauseKernel/IRQfindNextThread race).
 *
 * r0 = *ctxsave (pointer to the word array); store SP (r14) into [r0].
 */
#define saveContext()                                                         \
{                                                                             \
    asm volatile("lrw   r0, ctxsave      \n\t" /*r0 = &ctxsave (the ptr var)*/\
                 "ld.w  r0, (r0, 0)      \n\t" /*r0 = ctxsave (->word array)*/\
                 "st.w  r14, (r0, 0)     \n\t" /*ctxsave[0] = thread SP     */\
                 "sync                   \n\t" /*barrier (ARM dmb equiv)    */\
                 ::: "r0", "memory");                                         \
}

/**
 * \internal
 * restoreContext(): load the (possibly new) thread SP from *ctxsave back
 * into r14, so the stub's subsequent ldm+rte reloads the chosen thread's
 * frame.  Ref A sec.1 contract: must run LAST and trigger return-from-exc.
 */
#define restoreContext()                                                      \
{                                                                             \
    asm volatile("lrw   r0, ctxsave      \n\t"                                \
                 "ld.w  r0, (r0, 0)      \n\t" /*r0 = ctxsave (->word array)*/\
                 "ld.w  r14, (r0, 0)     \n\t" /*thread SP = ctxsave[0]     */\
                 ::: "r0", "memory");                                         \
}

namespace miosix_private {

/**
 * \internal Cause a context switch (cooperative yield).  Executes the yield
 * trap; the core vectors via VBR to the yield entry stub (Ref B sec.5).
 * trap #0 clobbers nothing of ours but we mark memory to bar reordering.
 */
inline void doYield()
{
    asm volatile("trap 0" ::: "memory");
}

/**
 * \internal Disable maskable interrupts: clear PSR.IE (Ref B sec.3).
 * Faults need not be disabled (matches cortexM0 PRIMASK semantics).
 * The empty asm barrier prevents the compiler reordering across the
 * critical-section boundary (Ref A sec.6).
 */
inline void doDisableInterrupts()
{
    asm volatile("psrclr ie");
    asm volatile("" ::: "memory");
}

/**
 * \internal Enable maskable interrupts: set PSR.IE (Ref B sec.3).
 */
inline void doEnableInterrupts()
{
    asm volatile("psrset ie");
    asm volatile("" ::: "memory");
}

/**
 * \internal \return true if maskable interrupts are enabled.
 * Reads PSR and tests the IE bit (Ref B sec.3 checkAreInterruptsEnabled).
 * Mirrors cortexM0: returns false when disabled.
 */
inline bool checkAreInterruptsEnabled()
{
    unsigned int psr;
    asm volatile("mfcr %0, psr" : "=r"(psr));
    return ((psr >> HD2_PSR_IE_BIT) & 1u) != 0u;
}

} //namespace miosix_private

#endif //PORTABILITY_IMPL_H
