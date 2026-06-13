/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CK803S (C-SKY V2) interrupt/trap entry stubs for the Miosix port.
 * STAGING DRAFT -> lib/miosix-kernel/miosix/arch/cskyv2/cskyv2_vectors.S
 * (or fold into the board startup_miosix.S).
 *
 * Two naked vector entry points, both following the proven irq_entry.S
 * stm/ldm + rte template (Ref B sec.2) and the cortexM0 contract shape
 * "save full context -> bl ISR body -> restore -> exception-return"
 * (Ref A sec.3):
 *
 *   yield_isr_entry  -> VBR[HD2_YIELD_VEC]   (trap #0, the doYield path)
 *   timer2_isr_entry -> VBR[34]              (Timer2, the preemption tick)
 *
 * ===================== CONTEXT-SWITCH FRAME =====================
 * CK803S HW auto-saves ONLY PC->EPC and PSR->EPSR on entry; it pushes
 * NOTHING to the stack (Ref B sec.5).  So this stub manually pushes the
 * FULL register set the kernel must preserve across an arbitrary-point
 * preemption.  Frame is 80 bytes = CTXSAVE_ON_STACK (arch_settings.h),
 * built on the *interrupted thread's* stack, full-descending:
 *
 *   offset  content              (from post-push SP, low->high address)
 *   ------  -------
 *   +0x00   r0   (a0)            volatile, also threadLauncher arg0
 *   +0x04   r1   (a1)            volatile, also threadLauncher arg1
 *   +0x08   r2   (a2)
 *   +0x0c   r3   (a3)
 *   +0x10   r4   (l0)  callee-saved
 *   +0x14   r5   (l1)  callee-saved
 *   +0x18   r6   (l2)  callee-saved
 *   +0x1c   r7   (l3)  callee-saved
 *   +0x20   r8   (l4)  callee-saved
 *   +0x24   r9   (l5)  callee-saved
 *   +0x28   r10  (l6)  callee-saved
 *   +0x2c   r11  (l7)  callee-saved
 *   +0x30   r12  (t0)  volatile
 *   +0x34   r13  (t1)  volatile
 *   +0x38   r16  (l8)  callee-saved
 *   +0x3c   r17  (l9)  callee-saved
 *   +0x40   r15  (lr)  return address of interrupted code
 *   +0x44   r28  (gb)  GBR (saved for safety per Ref B sec.1)
 *   +0x48   EPC        saved exception PC  (resume address)
 *   +0x4c   EPSR       saved exception PSR (resume IE/EE/cond state)
 *   ------  total = 0x50 = 80 bytes
 *
 * Carrying EPC/EPSR IN the frame (not relying on the live cr4/cr5 surviving)
 * is what makes resume UNIFORM between an already-running thread and a
 * brand-new one: CTX_SAVE snapshots EPC/EPSR (the HW-saved resume point) into
 * the frame; CTX_RESTORE writes them back to cr4/cr5 with mtcr just before
 * rte.  For a NEW thread, initCtxsave() pre-fills +0x48 = threadLauncher and
 * +0x4c = a PSR with IE+EE set -- so the first rte jumps straight into
 * threadLauncher with interrupts enabled and r0/r1 already loaded by ldm.
 * This removes the OPEN-QUESTION fragility of seeding EPC out-of-band.
 *
 * NOTE on r18-r25 (t2-t9, volatile): a *cooperative* yield (trap) does not
 * need them (the C call to doYield already clobbered them).  A *preemptive*
 * tick CAN interrupt arbitrary code that holds live values in r18-r25.  For
 * Phase-1 correctness we accept this limitation ONLY because the C body
 * itself is the sole code that runs between save and restore, and it will
 * re-clobber/restore per the ABI -- BUT see OPEN QUESTIONS in BUILD_NOTES.md:
 * a fully-correct preemptive port must also save r18-r25 (extend frame to
 * 0x70).  We keep r0-r17 + r28 here to match the cooperative+tick paths that
 * route through C; widen before trusting preemption of hand-asm code.
 * (PSR/PC are already in EPSR/EPC; rte restores them -- no manual save.)
 * ================================================================
 *
 * Why ctxsave[0] = SP works for the FIRST switch into a new thread:
 * initCtxsave() (portability.cpp) lays down an identical 80-byte frame on
 * the new thread's stack with r0=func, r1=argv, r15=sentinel, and arranges
 * EPC=threadLauncher / EPSR=IE-set.  ctxsave[0] points at it.  The first
 * restoreContext loads that SP, ldm pops the frame, rte jumps to EPC
 * (threadLauncher) with r0/r1 set per the C-SKY ABI (Ref B sec.1).
 */

    .equ FRAME_SIZE, 0x50      /* 80 bytes, == CTXSAVE_ON_STACK */
    .equ OFF_R15,    0x40
    .equ OFF_R28,    0x44
    .equ OFF_EPC,    0x48
    .equ OFF_EPSR,   0x4c

    .text
    .align  2

/* -------------------------------------------------------------------- *
 *  Shared save macro: push full frame onto current (thread) SP, then    *
 *  publish SP into ctxsave[0].                                          *
 * -------------------------------------------------------------------- */
    .macro  CTX_SAVE
    subi    sp, sp, FRAME_SIZE
    stm     r0-r13, (sp)            /* r0..r13 -> +0x00..+0x34 (Ref B sec.2) */
    st.w    r16, (sp, 0x38)
    st.w    r17, (sp, 0x3c)
    st.w    r15, (sp, OFF_R15)      /* LR of interrupted code */
    st.w    r28, (sp, OFF_R28)      /* GBR */
    mfcr    r0, cr<4, 0>                 /* HW-saved resume PC  (cr4) */
    st.w    r0, (sp, OFF_EPC)
    mfcr    r0, cr<2, 0>                /* HW-saved resume PSR (cr2) */
    st.w    r0, (sp, OFF_EPSR)
    /* publish SP into *ctxsave (cur->ctxsave[0]) */
    lrw     r0, ctxsave             /* r0 = &ctxsave (the pointer variable) */
    ld.w    r0, (r0, 0)             /* r0 = ctxsave (-> word array) */
    st.w    sp, (r0, 0)             /* ctxsave[0] = thread SP */
    sync                            /* barrier == ARM dmb (Ref A sec.1) */
    .endm

/* -------------------------------------------------------------------- *
 *  Shared restore macro: reload SP from ctxsave[0] (the scheduler may   *
 *  have repointed ctxsave at a DIFFERENT thread), pop the frame, rte.   *
 * -------------------------------------------------------------------- */
    .macro  CTX_RESTORE
    lrw     r0, ctxsave
    ld.w    r0, (r0, 0)             /* r0 = ctxsave (-> chosen thread array) */
    ld.w    sp, (r0, 0)             /* SP = chosen thread's ctxsave[0] */
    ld.w    r0, (sp, OFF_EPSR)
    mtcr    r0, cr<2, 0>                /* cr2 <- resume PSR (rte restores from here) */
    ld.w    r0, (sp, OFF_EPC)
    mtcr    r0, cr<4, 0>                 /* cr4 <- resume PC  (threadLauncher for new thr) */
    ld.w    r28, (sp, OFF_R28)
    ld.w    r15, (sp, OFF_R15)
    ld.w    r17, (sp, 0x3c)
    ld.w    r16, (sp, 0x38)
    ldm     r0-r13, (sp)            /* restore r0..r13 (r0=func,r1=argv for new thr) */
    addi    sp, sp, FRAME_SIZE
    rte                             /* PC<-EPC, PSR<-EPSR; re-enables IE (Ref B sec.5) */
    .endm

/* ==================== Timer2 tick = PREEMPT path ==================== */
    .globl  timer2_isr_entry
    .type   timer2_isr_entry, @function
timer2_isr_entry:
    CTX_SAVE
    /* miosix_private::ISR_preempt() -- C++ mangled (Ref A sec.3/4) */
    jbsr    _ZN14miosix_private11ISR_preemptEv
    CTX_RESTORE
    .size   timer2_isr_entry, .-timer2_isr_entry

/* ==================== trap #0 = YIELD path ========================= */
    .globl  yield_isr_entry
    .type   yield_isr_entry, @function
yield_isr_entry:
    CTX_SAVE
    /* miosix_private::ISR_yield() -- C++ mangled (Ref A sec.3/4) */
    jbsr    _ZN14miosix_private9ISR_yieldEv
    CTX_RESTORE
    .size   yield_isr_entry, .-yield_isr_entry
