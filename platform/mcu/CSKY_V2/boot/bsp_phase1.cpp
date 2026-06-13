/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal Phase-1 board support for the Ailunce HD2 Miosix bring-up.
 * STAGING DRAFT -> platform/mcu/CSKY_V2/boot/bsp_phase1.cpp (test board).
 *
 * Implements the four bsp.h hooks (Ref C sec.3) in namespace miosix:
 *   IRQbspInit()  - pre-kernel, IRQ-disabled: console UART + GPIO banks.
 *                   (clock/PLL already up via our platform; the tick timer
 *                    is set up in the ARCH layer's IRQportableStartKernel,
 *                    NOT here -- there is no SysTick on CK803S, Ref C sec.3.)
 *   bspInit2()    - post-kernel: empty for Phase-1.
 *   shutdown()    - reboot via the arch hook.
 *   reboot()      - disable IRQs + arch reboot.
 *
 * The LED GPIO pokes used by the test live here too (GREEN=GPIOB.0,
 * RED=GPIOB.1 -- memory project_hd2_leds_beeps).
 */

#include "interfaces/bsp.h"
#include "interfaces/portability.h"
#include "kernel/kernel.h"
#include <stdint.h>

namespace miosix {

/* ---- GPIOB (LEDs).  Address from the HD2 platform; named, no magics. ---- *
 * GPIOB DR (data) drives GREEN (bit0) and RED (bit1).  The exact GPIOB base
 * is the one already used by the working OpenRTX HD2 LED driver; we re-export
 * the two helpers here so the standalone test has no OpenRTX dependency.   */
#define HD2_GPIOB_BASE   0x14120000u      /* GPIOB bank (verify vs platform) */
#define HD2_GPIOB_DR     (*(volatile uint32_t *)(HD2_GPIOB_BASE + 0x00u))
#define HD2_GPIOB_DDR    (*(volatile uint32_t *)(HD2_GPIOB_BASE + 0x04u))
#define LED_GREEN_BIT    (1u << 0)        /* GPIOB.0 (memory leds_beeps) */
#define LED_RED_BIT      (1u << 1)        /* GPIOB.1 (memory leds_beeps) */

void ledGreenOn()  { HD2_GPIOB_DR |=  LED_GREEN_BIT; }
void ledGreenOff() { HD2_GPIOB_DR &= ~LED_GREEN_BIT; }
void ledRedOn()    { HD2_GPIOB_DR |=  LED_RED_BIT; }
void ledRedOff()   { HD2_GPIOB_DR &= ~LED_RED_BIT; }

/* ---- UART0 console for IRQbootlog/bootlog (Ref C sec.3) ---------------- *
 * The HD2 UART0 @ 57600 is already brought up by our platform clock init
 * (memory project_hd2_clock_init).  For Phase-1 we provide the byte sink
 * Miosix's bootlog path calls.  If the project already links a uart0 driver,
 * forward to it; otherwise this is a stub the linker resolves to the real
 * one.  Declared extern so we don't duplicate the driver here.            */
extern "C" void hd2_uart0_putc(char c);  //provided by the platform UART driver

} //namespace miosix

/*
 * Miosix routes IRQbootlog/bootlog through the default console device unless
 * WITH_BOOTLOG hooks are overridden.  The simplest Phase-1 path: define the
 * low-level char writer the default console calls.  (If the kernel build
 * expects IRQbootlog as a weak symbol, override it here.)
 */
namespace miosix {

void IRQbspInit()
{
    //Clock/PLL already up (platform). Console UART already at 57600.
    //Bring GPIOB LED pins to outputs (DDR bit=1 => output on DW GPIO).
    HD2_GPIOB_DDR |= (LED_GREEN_BIT | LED_RED_BIT);
    ledGreenOff();
    ledRedOff();
    //DO NOT enable interrupts here (Ref C sec.1: _init asserts IRQs off).
    //The tick timer is programmed by IRQportableStartKernel (arch layer).
}

void bspInit2()
{
    //Phase-1: nothing needed after the kernel starts.
}

void shutdown()
{
    //Park / reboot.  Must not return (Ref C sec.3).
    reboot();
}

void reboot()
{
    disableInterrupts();
    miosix_private::IRQsystemReboot();   //jumps to BOOTROM 0x4 (arch hook)
    for(;;) ;                            //unreachable
}

} //namespace miosix
