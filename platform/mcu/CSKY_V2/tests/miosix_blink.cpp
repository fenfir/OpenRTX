/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase-1 standalone validation: two LED-blink threads at different rates.
 * STAGING DRAFT -> platform/mcu/CSKY_V2/tests/miosix_blink.cpp.
 *
 * This is the application main() that mainLoader() calls AFTER the kernel is
 * already running (Ref C sec.1).  We do NOT call startKernel() ourselves --
 * crt0 -> _init() -> startKernel() does that, then mainLoader() runs THIS as
 * the main thread.  We just create two threads and let the scheduler run them.
 *
 * PASS CRITERION: GREEN (GPIOB.0) blinks at ~2 Hz and RED (GPIOB.1) blinks at
 * ~1.25 Hz INDEPENDENTLY and CONCURRENTLY.  Independent concurrent blinking is
 * only possible if Thread::sleep() (which yields) AND the Timer2 preemption
 * tick both work -- i.e. the context switch + tick are correct.  If only one
 * LED blinks, or they blink in lockstep, the switch/tick is broken.
 */

#include <miosix.h>

using namespace miosix;

/* LED pokes from bsp_phase1.cpp (GREEN=GPIOB.0, RED=GPIOB.1). */
namespace miosix {
void ledGreenOn();  void ledGreenOff();
void ledRedOn();    void ledRedOff();
}

static void blinkGreen(void *)
{
    for(;;)
    {
        ledGreenOn();
        Thread::sleep(250);   //250 ms on  (uses the tick -> getTick/sleep)
        ledGreenOff();
        Thread::sleep(250);   //250 ms off  => ~2 Hz
    }
}

static void blinkRed(void *)
{
    for(;;)
    {
        ledRedOn();
        Thread::sleep(400);
        ledRedOff();
        Thread::sleep(400);   //=> ~1.25 Hz, deliberately incommensurate w/ green
    }
}

int main()
{
    //Kernel is ALREADY running here (mainLoader created us as the main thread).
    Thread::create(blinkGreen, STACK_MIN);
    Thread::create(blinkRed,   STACK_MIN);

    //Main thread idles; the scheduler time-slices the two blinkers.
    for(;;) Thread::sleep(1000);
    return 0;
}
