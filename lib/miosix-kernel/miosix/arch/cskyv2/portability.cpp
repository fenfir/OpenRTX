/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Miosix portability.cpp for the CK803S (C-SKY V2) core (Ailunce HD2).
 * STAGING DRAFT -> lib/miosix-kernel/miosix/arch/cskyv2/portability.cpp.
 *
 * Implements the out-of-line half of the arch contract (Ref A sec.2,4,5,6):
 *   - the global `ctxsave` pointer
 *   - the noinline ISR bodies ISR_preempt() / ISR_yield()
 *   - IRQstackOverflowCheck()
 *   - initCtxsave()  (builds the 80-byte frame matching cskyv2_vectors.S)
 *   - IRQportableStartKernel()  (VBR + PIC + Timer2 @ TICK_FREQ, first yield)
 *   - IRQsystemReboot(), sleepCpu()
 *
 * The naked vector entry stubs live in cskyv2_vectors.S.
 */

#include "interfaces/portability.h"
#include "arch_registers_impl.h"   /* CK803S MMIO macros + cr accessors (see portability_impl.h note) */
#include "kernel/kernel.h"
#include "kernel/error.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/tick_interrupt.h"
#include "config/miosix_settings.h"

/*
 * Global current-thread ctxsave pointer.  Ref A sec.1: the macros read it,
 * IRQfindNextThread repoints it, IRQportableStartKernel seeds it.
 */
extern "C" {
volatile unsigned int *ctxsave;
}

/* asm entry stubs (cskyv2_vectors.S) */
extern "C" void timer2_isr_entry(void);
extern "C" void yield_isr_entry(void);

namespace miosix_private {

/*
 * Stack-overflow watermark check, called at the top of every ISR body.
 * Verbatim contract from Ref A sec.4 -- depends on cur->ctxsave[0] being
 * the thread SP, which our design guarantees (arch_settings.h CTXSAVE LAYOUT).
 */
void IRQstackOverflowCheck()
{
    const unsigned int watermarkSize = miosix::WATERMARK_LEN / sizeof(unsigned int);
    for(unsigned int i = 0; i < watermarkSize; i++)
    {
        if(miosix::cur->watermark[i] != miosix::WATERMARK_FILL)
            miosix::errorHandler(miosix::STACK_OVERFLOW);
    }
    if(miosix::cur->ctxsave[0] < reinterpret_cast<unsigned int>(
            miosix::cur->watermark + watermarkSize))
        miosix::errorHandler(miosix::STACK_OVERFLOW);
}

/*
 * Preemption-tick ISR body (Timer2).  noinline so it is not inlined into
 * the naked stub and not optimized away (Ref A sec.4).  Order:
 *   1. clear the HW interrupt source (BOTH writes, Ref B sec.6) -- do this
 *      FIRST so a re-entry isn't pending when we (maybe) switch threads.
 *   2. stack-overflow check
 *   3. miosix::IRQtickInterrupt() -- wakes sleepers, runs the scheduler,
 *      and (under SCHED_TYPE_PRIORITY) calls IRQfindNextThread() which may
 *      repoint the global `ctxsave` at a new thread.  The stub's CTX_RESTORE
 *      then reloads SP from the (possibly new) ctxsave[0].
 */
void ISR_preempt() __attribute__((noinline));
void ISR_preempt()
{
    (void)HD2_T2_EOI;          /* read clears the Timer2 request (Ref B sec.6) */
    HD2_PIC_COW1 = 4u;         /* PIC interrupt-end (Ref B sec.6) */

    IRQstackOverflowCheck();
    miosix::IRQtickInterrupt();
}

/*
 * Cooperative-yield ISR body (trap #0).  noinline.  No HW source to clear
 * (a trap is not a PIC line).  Just select the next thread.  Ref A sec.4.
 */
void ISR_yield() __attribute__((noinline));
void ISR_yield()
{
    IRQstackOverflowCheck();
    miosix::Scheduler::IRQfindNextThread();
}

/*
 * Build the initial context for a new thread (Ref A sec.2).  We lay down an
 * 80-byte frame IDENTICAL in layout to what cskyv2_vectors.S CTX_SAVE pushes,
 * so the first CTX_RESTORE pops it cleanly and rte lands in threadLauncher.
 *
 * C-SKY ABI (Ref B sec.1): arg0 = r0(a0), arg1 = r1(a1).  threadLauncher's
 * signature is threadLauncher(threadfunc, argv), so:
 *   r0 = pc   (the user thread entry function)   <- arg0
 *   r1 = argv                                     <- arg1
 *
 * RESUME IS UNIFORM (no EPC fragility): the stub carries saved EPC/EPSR in
 * the frame (+0x48/+0x4c) and writes them back to cr4/cr2 before rte.  For a
 * NEW thread we just pre-fill those two slots: EPC = threadLauncher, EPSR =
 * a PSR with IE+EE set.  The first CTX_RESTORE then ldm-loads r0=func,
 * r1=argv and rte jumps into threadLauncher(func,argv) with IRQs enabled --
 * matching threadLauncher's C-SKY-ABI signature (Ref A sec.2, Ref B sec.1).
 */
void initCtxsave(unsigned int *ctxsave, void *(*pc)(void *), unsigned int *sp,
        void *argv)
{
    //Full-descending: reserve the 80-byte frame (FRAME_SIZE in the stub).
    unsigned int *f = sp;
    f -= (80 / sizeof(unsigned int));   //20 words

    //Frame layout MUST match cskyv2_vectors.S CTX_SAVE exactly:
    f[0]  = reinterpret_cast<unsigned long>(pc);    // r0 = a0 = thread func
    f[1]  = reinterpret_cast<unsigned long>(argv);  // r1 = a1 = argv
    f[2]  = 0;                                       // r2
    f[3]  = 0;                                       // r3
    f[4]  = 0;  f[5]  = 0;  f[6]  = 0;  f[7]  = 0;    // r4-r7
    f[8]  = 0;  f[9]  = 0;  f[10] = 0;  f[11] = 0;    // r8-r11
    f[12] = 0;  f[13] = 0;                            // r12,r13
    f[14] = 0;  f[15] = 0;                            // r16,r17  (+0x38,+0x3c)
    // +0x40 (word 16): saved LR.  threadLauncher never returns, so a sentinel
    // is fine; use threadLauncher too so a stray return spins predictably.
    f[16] = reinterpret_cast<unsigned long>(&miosix::Thread::threadLauncher);
    // +0x44 (word 17): saved GBR.  Single static image -> the boot-time GBR.
    {
        unsigned int gbr;
        asm volatile("mov %0, r28" : "=r"(gbr));
        f[17] = gbr;
    }
    // +0x48 (word 18): saved EPC -> threadLauncher (rte jumps here first run).
    f[18] = reinterpret_cast<unsigned long>(&miosix::Thread::threadLauncher);
    // +0x4c (word 19): saved EPSR -> IE+EE set so the thread runs with IRQs on.
    f[19] = (1u << HD2_PSR_IE_BIT) | (1u << HD2_PSR_EE_BIT);

    ctxsave[0] = reinterpret_cast<unsigned long>(f); // ctxsave[0] = thread SP
    ctxsave[1] = 0;                                  // reserved
}

/* -------- VBR table + Timer2/PIC tick setup (Ref B sec.4/6) ---------- *
 * We build our OWN 64-entry VBR table in .bss (the IAP's lives in low IRAM
 * we don't own -- Ref B/C boot notes).  All vectors -> a spin trap stub,
 * then overwrite the two live ones.                                      */

static unsigned int g_vbr_table[64] __attribute__((aligned(1024)));

extern "C" void miosix_cskyv2_trap_catch(void);
asm(
    "   .text                       \n"
    "   .align 2                    \n"
    "   .globl miosix_cskyv2_trap_catch\n"
    "miosix_cskyv2_trap_catch:      \n"
    "1: br 1b                       \n"   /* spin so a stray vector is observable */
);

void IRQportableStartKernel()
{
    //1. Build + install our VBR table.
    for(int i = 0; i < 64; i++)
        g_vbr_table[i] = reinterpret_cast<unsigned int>(&miosix_cskyv2_trap_catch);
    g_vbr_table[HD2_TIMER2_VEC] = reinterpret_cast<unsigned int>(&timer2_isr_entry);
    g_vbr_table[HD2_YIELD_VEC]  = reinterpret_cast<unsigned int>(&yield_isr_entry);
    csky_set_vbr(g_vbr_table);

    //2. PIC: mask all, unmask Timer2 (bit=0 enables).  Ref B sec.6 recipe.
    HD2_PIC_MODE    = 0u;
    HD2_PIC_PO      = 0xFFFFFFFFu;
    HD2_PIC_MODE1   = 0u;
    HD2_PIC_PO1     = 0xFFFFFFFFu;
    HD2_PIC_INT_ST  = 0xFFFFFFFFu;
    HD2_PIC_INT_ST1 = 0xFFFFFFFFu;
    HD2_PIC_MASK    = 0xFFFFFFFFu;
    HD2_PIC_MASK   &= ~(1u << HD2_TIMER2_SRC);

    //3. Timer2 periodic @ miosix::TICK_FREQ (Ref A sec.5 uses TICK_FREQ).
    HD2_T2_CTRL  = 2u;                                  //reload mode, disabled
    HD2_T2_LOAD  = HD2_TIMER_HZ / miosix::TICK_FREQ;    //e.g. 42e6/1000 = 42000
    HD2_T2_CTRL |= 1u;                                  //enable|reload|int (=3)

    //4. Scratch ctxsave for the very first saveContext (Ref A sec.5 step 4).
    unsigned int s_ctxsave[miosix::CTXSAVE_SIZE];
    ctxsave = s_ctxsave;

    //5. Raw enable (NOT enableInterrupts() -- Ref A sec.5 step 5).  Set EE+IE.
    asm volatile("psrset ee, ie");

    //6. First switch: yield into the first real thread.  Never returns.
    miosix::Thread::yield();
}

void IRQsystemReboot()
{
    //memory project_hd2_bootrom_reset: jump to BOOTROM 0x4, not 0x03000000.
    asm volatile(
        "lrw  r0, %0   \n\t"
        "jmp  r0       \n\t"
        :: "i"(HD2_BOOTROM_RESET) : "r0");
    for(;;) ; //unreachable
}

void sleepCpu()
{
    //CK803S low-power wait.  'wait' = doze until next interrupt (Ref B sec.6).
    asm volatile("wait");
}

} //namespace miosix_private
