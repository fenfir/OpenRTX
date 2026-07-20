/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Glue between OpenRTX and the (tickless) vendor Miosix kernel for the HD2.
 *
 * OpenRTX's interfaces/delays.h is implemented per-MCU on top of the kernel
 * primitives.  The vendor kernel is TICKLESS: getTime() returns nanoseconds
 * and replaces the old getTick(); sleepUntil is nanoSleepUntil(absNs).
 * OpenRTX works in ms (TICK_FREQ=1000), so convert at the boundary.
 */

#include <miosix.h>

// The kernel's delayMs/delayUs live in namespace miosix; OpenRTX declares the
// GLOBAL delayMs/delayUs.  Forward-declare the kernel symbols and wrap them.
namespace miosix
{
void delayMs(unsigned int);
void delayUs(unsigned int);
}

extern "C" {

// Raw UART0 trace for bring-up (UART0 @ 0x14030000, 57600 8N1 set by the bsp).
// Bounded spin so a stuck UART can't hang a thread.
void hd2_trace(const char *s)
{
    volatile unsigned int *thr = (volatile unsigned int *)0x14030000u;
    volatile unsigned int *lsr = (volatile unsigned int *)0x14030014u;
    while (*s) {
        for (unsigned int g = 0; g < 200000u && (*lsr & 0x20u) == 0u; ++g) {
        }
        *thr = (unsigned char)*s++;
    }
}

// OpenRTX delays interface (global symbols OpenRTX code calls).
void delayMs(unsigned int ms)
{
    miosix::delayMs(ms);
}
void delayUs(unsigned int us)
{
    miosix::delayUs(us);
}

void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    miosix::Thread::sleep(seconds * 1000u + mseconds);
}

void sleepUntil(long long timestampMs)
{
    miosix::Thread::nanoSleepUntil(timestampMs * 1000000LL);
}

long long getTick(void)
{
    return miosix::getTime() / 1000000LL; // ns -> ms (TICK_FREQ=1000)
}

} // extern "C"
