/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miosix arch_settings.h for the T-Head CK803S (C-SKY V2) core (Ailunce HD2).
 * STAGING DRAFT -- belongs at lib/miosix-kernel/miosix/arch/cskyv2/arch_settings.h.
 *
 * These three constants size Thread::ctxsave[] and tell the kernel how much
 * the arch stashes on each thread's own stack. They MUST interlock exactly
 * with portability_impl.h (saveContext/restoreContext) and portability.cpp
 * (initCtxsave) -- see CTXSAVE LAYOUT below and BUILD_NOTES.md.
 *
 * ============================ CTXSAVE LAYOUT ============================
 * Unlike ARM Cortex-M, CK803S does NOT auto-stack registers on exception
 * entry (Ref B sec.5: HW only saves PC->EPC, PSR->EPSR, clears IE).  So our
 * design splits context like irq_entry.S already does:
 *
 *   - The ISR ENTRY STUB (cskyv2_vectors.S) pushes the FULL interrupt frame
 *     onto the *interrupted thread's own stack* (volatile + callee-saved +
 *     LR + saved PSR), 80 bytes.  This is the "CTXSAVE_ON_STACK" region.
 *   - ctxsave[] holds exactly ONE word: ctxsave[0] = the thread SP that
 *     points at the top of that on-stack frame.
 *
 * This mirrors the cortexM0 contract "ctxsave[0] is always the thread SP"
 * (Ref A sec.0) and keeps IRQstackOverflowCheck working (it compares
 * cur->ctxsave[0] against the watermark top -- Ref A sec.4).  The actual
 * register values live on the stack, restored by the stub's ldm+rte.
 * =======================================================================
 */

#ifndef ARCH_SETTINGS_H
#define ARCH_SETTINGS_H

namespace miosix {

/*
 * Number of unsigned int words in Thread::ctxsave[].  We store ONLY the
 * thread SP here (everything else lives in the on-stack frame the entry
 * stub builds), but we round up to 2 so the array stays 8-byte sized and
 * leaves room for a future GBR/extra slot without resizing the TCB.
 * ctxsave[0] = thread SP (load-bearing: read by IRQstackOverflowCheck and
 * written by initCtxsave / the entry stub).  ctxsave[1] = unused/reserved.
 */
const unsigned char CTXSAVE_SIZE = 2;

/*
 * Bytes the arch stashes on the thread's own stack during a context save.
 * This is the size of the interrupt frame the entry stub pushes (sec.
 * "CTXSAVE LAYOUT" above and cskyv2_vectors.S): 18 GPR words + LR + PSR,
 * padded to 8-byte alignment = 80 bytes.  The kernel adds this to every
 * thread's stack size so a preemption never overflows.  MUST be divisible
 * by 4 (Ref A sec.0).  See FRAME_WORDS in cskyv2_vectors.S -- keep equal.
 */
const unsigned int CTXSAVE_ON_STACK = 80;

/*
 * Required stack alignment in bytes.  C-SKY V2 ABI: SP must always be
 * 8-byte aligned (Ref B sec.1).  Same value as ARM AAPCS.
 */
const unsigned int CTXSAVE_STACK_ALIGNMENT = 8;

} //namespace miosix

#endif //ARCH_SETTINGS_H
