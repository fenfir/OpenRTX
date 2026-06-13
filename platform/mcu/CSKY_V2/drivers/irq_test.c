/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal periodic-timer-interrupt bring-up test for the HD2
 * (HR_C7000 / CK803S).  This is the deciding experiment for the
 * loader->OpenRTX-app switch: can we get a clean periodic IRQ after the
 * IAP handoff?  If yes, a preemptive RTOS (miosix) C-SKY port is viable;
 * if not, we take the cooperative superloop.
 *
 * Recipe reverse-engineered from the vendor V2.1.3 firmware (which runs
 * uC/OS-III with a 100 Hz PIC-driven autovectored tick).  See memory
 * project-hd2-interrupt-recipe.  Mechanism:
 *
 *   timer IRQ -> external PIC @ 0x17000000 (level, active-high)
 *             -> CK803 autovectors via VBR (cr<1,0>)
 *             -> vector = 32 + PIC_source.  Timer2 = src2 = vector 34.
 *
 * We keep ch0/Timer1 as the free-running getTick timebase (delays.c) and
 * use ch1/Timer2 here for the periodic tick IRQ, so the two don't fight.
 *
 * Clear sequence (BOTH parts required):
 *   - read TimerN_EOI @ base+0x0c  (a READ clears the timer request)
 *   - write PIC_COW1 @ 0x17000010 = 4  (PIC interrupt-end)
 */

#include <stdint.h>

/* ---- DW_apb_timers (0x14000000), ch1 = Timer2 @ +0x14 -------------- */
#define TIMER_BASE     0x14000000u
#define TMR(ch, off)   (*(volatile uint32_t *)(TIMER_BASE + (ch) * 0x14u + (off)))
#define T2_LOAD        TMR(1u, 0x00u)
#define T2_CTRL        TMR(1u, 0x08u)
#define T2_EOI         TMR(1u, 0x0cu)   /* read clears Timer2 IRQ */

/* ---- PIC interrupt controller (0x17000000) ------------------------- */
#define PIC_BASE       0x17000000u
#define PIC_MODE       (*(volatile uint32_t *)(PIC_BASE + 0x00u))
#define PIC_PO         (*(volatile uint32_t *)(PIC_BASE + 0x04u))
#define PIC_MASK       (*(volatile uint32_t *)(PIC_BASE + 0x08u))  /* bit=0 ENABLES */
#define PIC_COW1       (*(volatile uint32_t *)(PIC_BASE + 0x10u))  /* interrupt-end */
#define PIC_INT_ST     (*(volatile uint32_t *)(PIC_BASE + 0x44u))
#define PIC_INT_ST1    (*(volatile uint32_t *)(PIC_BASE + 0x48u))
#define PIC_MODE1      (*(volatile uint32_t *)(PIC_BASE + 0x60u))
#define PIC_PO1        (*(volatile uint32_t *)(PIC_BASE + 0x64u))

#define TIMER2_SRC     2u
#define TIMER2_VEC     34u
#define TIMER_HZ       42000000u
#define TICK_HZ        100u             /* match the vendor's 100 Hz tick */

static volatile uint32_t s_irq_ticks;
static int s_inited;

/* VBR table: 64 word entries (vec 0..63), each a handler address.
 * CK803 loads the word at VBR + vec*4 and jumps to it.  Aligned 1 KiB
 * to satisfy the VBR alignment requirement. */
static uint32_t g_vbr_table[64] __attribute__((aligned(1024)));

/* Catch-all for any unexpected exception/IRQ -- spin so a stray vector
 * is observable (radio wedges) rather than running wild. */
static void trap_stub(void)
{
    for (;;)
        ;
}

/* Plain C worker for the Timer2 IRQ.  Called by the asm entry stub
 * timer2_isr_entry (irq_entry.S), which does the save/restore + rte --
 * GCC's interrupt attribute is ignored by our toolchain, so we can't
 * rely on it here.  This returns normally (jmp r15) back to the stub. */
void timer2_isr_body(void)
{
    (void)T2_EOI;          /* read clears the Timer2 request */
    s_irq_ticks++;
    PIC_COW1 = 4u;         /* PIC interrupt-end */
}

/* The actual vector entry (asm): saves context, calls timer2_isr_body, rte. */
extern void timer2_isr_entry(void);

uint32_t irq_test_count(void)
{
    return s_irq_ticks;
}

void irq_test_init(void)
{
    if (s_inited)
        return;
    s_inited = 1;

    /* 1. Build + install our own VBR table (the IAP's lives in low IRAM
     *    we don't own).  All vectors -> trap_stub except Timer2. */
    for (int i = 0; i < 64; i++)
        g_vbr_table[i] = (uint32_t)&trap_stub;
    g_vbr_table[TIMER2_VEC] = (uint32_t)&timer2_isr_entry;
    __asm__ volatile ("mtcr %0, cr<1, 0>" :: "r"(g_vbr_table));

    /* 2. PIC init (mask all first), then unmask Timer2. */
    PIC_MODE    = 0u;             /* srcs 0-31 level-triggered */
    PIC_PO      = 0xFFFFFFFFu;    /* active-high */
    PIC_MODE1   = 0u;
    PIC_PO1     = 0xFFFFFFFFu;
    PIC_INT_ST  = 0xFFFFFFFFu;    /* clear pending */
    PIC_INT_ST1 = 0xFFFFFFFFu;
    PIC_MASK    = 0xFFFFFFFFu;    /* mask everything */
    PIC_MASK   &= ~(1u << TIMER2_SRC);   /* unmask Timer2 (bit=0 enables) */

    /* 3. Program Timer2 periodic @ TICK_HZ. */
    T2_CTRL  = 2u;                       /* user-defined/reload, disabled */
    T2_LOAD  = TIMER_HZ / TICK_HZ;       /* 100 Hz */
    T2_CTRL |= 1u;                       /* enable | reload | int-unmasked (=3) */

    /* 4. Global interrupt enable LAST (PSR already has EE from reset). */
    __asm__ volatile ("psrset ee, ie");
}
