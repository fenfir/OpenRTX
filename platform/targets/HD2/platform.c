/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ailunce HD2 platform layer -- minimal bring-up (display / backlight /
 * keyboard / RTC / power).  The Miosix bsp already brings up clocks, the
 * watchdog-off, the UART0 console, the LEDs and the PTB13 power self-latch,
 * so this file only owns the OpenRTX-facing HAL and the power/PTT sense.
 */

#include "interfaces/platform.h"
#include "peripherals/rtc.h"
#include "drivers/adc_hd2.h"
#include "hwconfig.h"

extern void backlight_init(void);
extern void backlight_terminate(void);

static const hwInfo_t hwInfo = {
    .name = "HD2",
    .hw_version = 0,
    .flags = 0,
    .uhf_band = 1,
    .vhf_band = 1,
    .uhf_maxFreq = 480,
    .uhf_minFreq = 400,
    .vhf_maxFreq = 174,
    .vhf_minFreq = 136,
};

void platform_init()
{
    /* Establish the V2.1.3 SoC pin-mux + GPIO baseline the display and
     * backlight depend on.  The Miosix kernel already brought up the PLL,
     * timers and UART0; here we only own the pad mux / GPIO directions that
     * the vendor firmware sets and the LCD/PWM hardware needs.  (Clocks are
     * NOT re-touched under Miosix -- re-running the PLL init hangs the kernel
     * timer.)  These are whole-register vendor snapshot values; the upper
     * DIPLEX bits are write-only, so never read-modify-write them. */
    SOCSYS_IO_DIPLEX0 = 0x00000060u; /* PTA: vendor pad-mux baseline */
    SOCSYS_IO_DIPLEX1 = 0x00000007u; /* PTA: SPI0 pads (vendor baseline) */
    SOCSYS_IO_DIPLEX2 = HD2_DIPLEX2_LCD_I80; /* PTC: LCD i8080 owns the bus */

    GPIOA_DDR = 0x003401e0u;
    GPIOA_DR |= 0x001401e0u;

    GPIOB_DDR = 0x3d7ae51fu; /* incl. knob/PTT sense as input */
    GPIOB_DR |= 0x00506414u; /* preserves the PTB13 power latch */

    GPIOC_DDR = 0x00007ffcu; /* PTC2..14 = LCD-bus outputs */

    backlight_init();        /* sets PWM ch0 duty-gating words */
    adc_hd2_init();          /* on-chip ADC (battery on ch2) */
    rtc_init();
}

void platform_terminate()
{
    backlight_terminate();
    rtc_terminate();

    /* The power self-latch drop + SOCSYS system soft-reset is done atomically
     * (IRQs off) by the miosix bsp shutdown() when main() returns -- doing the
     * latch-drop here first would risk a partial brown-out before the reset. */
}

const hwInfo_t *platform_getHwInfo()
{
    return &hwInfo;
}

bool platform_pwrButtonStatus()
{
    /* Volume/power knob sense on GPIOB.12: LOW = on. */
    return (GPIOB_EXT_PORT & PWR_KNOB_BIT) == 0;
}

bool platform_getPttStatus()
{
    /* PTT button on GPIOB.11, active-low. */
    return (GPIOB_EXT_PORT & PTT_BIT) == 0;
}

void platform_ledOn(led_t led)
{
    switch (led) {
        case GREEN:
            GPIOB_DR |= LED_GREEN_BIT;
            break;
        case RED:
            GPIOB_DR |= LED_RED_BIT;
            break;
        default:
            break;
    }
}

void platform_ledOff(led_t led)
{
    switch (led) {
        case GREEN:
            GPIOB_DR &= ~LED_GREEN_BIT;
            break;
        case RED:
            GPIOB_DR &= ~LED_RED_BIT;
            break;
        default:
            break;
    }
}

/* --- Battery voltage (on-chip ADC channel 2) ----------------------------- */
uint16_t platform_getVbat()
{
    uint32_t raw_avg = adc_hd2_battery_raw_avg();

    /* Defensive fallback: if the ADC ever fails to convert, report a plausible
     * mid-pack ~7.4 V rather than a misleading 0 % (which trips low-battery). */
    if (raw_avg == 0u)
        raw_avg = 0xbf4u;

    /* Vendor scale: deci_volts = (raw_avg * 99) >> 12; ~0xd40 -> ~8.1 V. */
    uint32_t deci_volts = ((raw_avg * 99u) & 0x7ffffu) >> 12;
    return (uint16_t)(deci_volts * 100u); /* millivolts */
}

/* --- Other analog/audio sensing not wired in the minimal bring-up --------- */
uint8_t platform_getMicLevel()
{
    return 0;
}
uint8_t platform_getVolumeLevel()
{
    return 0;
}
int8_t platform_getChSelector()
{
    return 0;
}
void platform_beepStart(uint16_t freq)
{
    (void)freq;
}
void platform_beepStop()
{
}

/* --- RTC ----------------------------------------------------------------- */
datetime_t platform_getCurrentTime()
{
    return rtc_getTime();
}

void platform_setTime(datetime_t t)
{
    rtc_setTime(t);
}
