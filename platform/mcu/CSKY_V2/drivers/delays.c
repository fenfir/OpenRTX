/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Active HD2/CSKY V2 bring-up.
 *
 * Timer-backed delays + monotonic system tick for the HD2 (HR_C7000).
 *
 * Backed by the on-chip DesignWare DW_apb_timers block @ 0x14000000
 * (COMP_VER "2.02*", live-probed 2026-05-31).  The timer input clock was
 * measured empirically at 42.000 MHz (== PWM_TIMER_HZ in the target
 * main.c); a free-running 32-bit down-counter therefore ticks 42 counts
 * per microsecond / 42000 per millisecond.
 *
 * This replaces the earlier calibrated busy-loop (ITERS_PER_US), which
 * was ~3.6x off and drifted with -O level.  A real counter makes
 * delayUs/delayMs exact and gives the OpenRTX core a true getTick() +
 * sleepUntil() so the UI/RTX loops can pace themselves.
 *
 * NO interrupt is used here -- ch0 runs free-running with its interrupt
 * masked, and getTick() polls.  When we bring up a periodic tick IRQ
 * (VBR + PIC) for an RTOS, the tick will move into an ISR and this file
 * shrinks to the delay helpers.
 *
 * Channel choice: ch0.  At IAP hand-off ch0 is enabled in *periodic*
 * mode (LOAD=0x000cd140 = 840000 -> 20 ms / 50 Hz) with its interrupt
 * unmasked, but nothing services it (no VBR installed), so repurposing
 * it as our free-running timebase is safe.
 */

#include "interfaces/delays.h"
#include <stdint.h>

/* ---- DW_apb_timers register map (per-channel stride 0x14) ------------- */
#define TIMER_BASE        0x14000000u
#define TMR_REG(ch, off)  (*(volatile uint32_t *)(TIMER_BASE + (ch) * 0x14u + (off)))
#define TMR_LOAD(ch)      TMR_REG((ch), 0x00u)   /* LoadCount             */
#define TMR_CURVAL(ch)    TMR_REG((ch), 0x04u)   /* CurrentValue (counts down) */
#define TMR_CTRL(ch)      TMR_REG((ch), 0x08u)   /* ControlReg            */
#define TMR_EOI(ch)       TMR_REG((ch), 0x0cu)   /* read clears interrupt */

/* ControlReg bits */
#define TMR_CTRL_ENABLE   (1u << 0)
#define TMR_CTRL_MODE     (1u << 1)              /* 0 = free-run, 1 = user-defined */
#define TMR_CTRL_INT_MASK (1u << 2)              /* 1 = interrupt masked  */

#define TICK_CH           0u                     /* free-running timebase */
#define COUNTS_PER_US     42u                    /* 42 MHz, measured      */
#define COUNTS_PER_MS     42000u

/* Monotonic accumulator.  getTick()/the delay helpers fold the
 * free-running down-counter's deltas into s_acc_counts; this is correct
 * as long as we sample more often than one 32-bit wrap (2^32 / 42 MHz ~=
 * 102 s), which every OpenRTX loop does (>=200 Hz).  Single-threaded
 * (superloop) for now -- not reentrant; revisit if a preemptive RTOS
 * starts calling getTick() from multiple contexts. */
static uint64_t s_acc_counts;
static uint32_t s_last_cur;

void timer_init(void)
{
    TMR_CTRL(TICK_CH) = 0;                        /* disable to reload */
    TMR_LOAD(TICK_CH) = 0xFFFFFFFFu;
    /* enable, free-running (MODE=0), interrupt masked */
    TMR_CTRL(TICK_CH) = TMR_CTRL_ENABLE | TMR_CTRL_INT_MASK;
    s_last_cur   = TMR_CURVAL(TICK_CH);
    s_acc_counts = 0;
}

/* Fold elapsed down-counter delta into the monotonic accumulator and
 * return total elapsed counts since timer_init(). */
static uint64_t elapsed_counts(void)
{
    uint32_t cur   = TMR_CURVAL(TICK_CH);
    uint32_t delta = (s_last_cur - cur) & 0xFFFFFFFFu;   /* down-counter */
    s_last_cur     = cur;
    s_acc_counts  += delta;
    return s_acc_counts;
}

long long getTick(void)
{
    return (long long)(elapsed_counts() / COUNTS_PER_MS);
}

void delayUs(unsigned int useconds)
{
    /* Poll the raw counter directly so we don't perturb the getTick
     * accumulator.  useconds * 42 stays within 32 bits for any sane
     * sub-second delay (overflow only past ~102 s). */
    uint32_t start = TMR_CURVAL(TICK_CH);
    uint32_t need  = useconds * COUNTS_PER_US;
    while (((start - TMR_CURVAL(TICK_CH)) & 0xFFFFFFFFu) < need)
        ;
}

void delayMs(unsigned int mseconds)
{
    /* Chunk into 1 ms delayUs() calls to keep the count math in 32 bits
     * and to bound how long the getTick accumulator goes unsampled. */
    while (mseconds--)
        delayUs(1000u);
}

void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    while (seconds--)
        delayMs(1000u);
    delayMs(mseconds);
}

void sleepUntil(long long timestamp)
{
    /* No scheduler yet: busy-poll the tick until the deadline.  Past
     * deadlines return immediately (getTick is monotonic). */
    while (getTick() < timestamp)
        ;
}
