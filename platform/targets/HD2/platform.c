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
#include "peripherals/gpio.h"
#include "peripherals/rtc.h"
#include "drivers/ADC/adcHrc7000.h"
#include "drivers/GPS/gps_HD2.h"
#include "drivers/backlight/backlight.h"
#include "hwconfig.h"
#include "pinmap.h"

/* On-chip ADC.  Battery pack on channel 2, read through a 1:3 divider; the
 * cell reference is 3.3 V and the converter is 10-bit.  No mutex -- the state
 * task is the only sampler. */
#define ADC_VBAT_CH 2u
ADC_HRC7000_DEVICE_DEFINE(adc1, NULL, ADC_COUNTS_TO_UV(3300000, 10))

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
    SOCSYS->IO_DIPLEX0 = 0x00000060u; /* PTA: vendor pad-mux baseline */
    SOCSYS->IO_DIPLEX1 = 0x00000007u; /* PTA: SPI0 pads (vendor baseline) */
    SOCSYS->IO_DIPLEX2 = HD2_DIPLEX2_LCD_I80; /* PTC: LCD i8080 owns the bus */

    GPIOA->DDR = 0x003401e0u;
    GPIOA->DR |= 0x001401e0u;

    GPIOB->DDR = 0x3d7ae51fu; /* incl. knob/PTT sense as input */
    GPIOB->DR |= 0x00506414u; /* preserves the PTB13 power latch */

    GPIOC->DDR = 0x00007ffcu; /* PTC2..14 = LCD-bus outputs */

    /* The backlight PWM is brought up by the display driver (display_init),
     * so it is not initialised here; the core sets the level afterwards. */
    adcHrc7000_init(&adc1); /* on-chip ADC (battery on ch2) */
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
    return gpio_readPin(PWR_SW) == 0; /* knob LOW = on */
}

bool platform_getPttStatus()
{
    return gpio_readPin(PTT_SW) == 0; /* active-low */
}

void platform_ledOn(led_t led)
{
    switch (led) {
        case GREEN:
            gpio_setPin(GREEN_LED);
            break;
        case RED:
            gpio_setPin(RED_LED);
            break;
        default:
            break;
    }
}

void platform_ledOff(led_t led)
{
    switch (led) {
        case GREEN:
            gpio_clearPin(GREEN_LED);
            break;
        case RED:
            gpio_clearPin(RED_LED);
            break;
        default:
            break;
    }
}

/* --- Battery voltage (on-chip ADC channel 2) ----------------------------- */
uint16_t platform_getVbat()
{
    /* adc_getVoltage returns the pin voltage in uV; the pack is behind a 1:3
     * divider, so multiply by 3 and convert uV -> mV.  The state update task
     * low-pass filters this, so no averaging is done here. */
    return (uint16_t)((adc_getVoltage(&adc1, ADC_VBAT_CH) * 3u) / 1000u);
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
/* Beep / keytone.  On the HD2 the beep is generated as a PCM tone through the
 * codec-DAC output stream by the voice-prompt player (core/voicePrompts_adpcm.c,
 * beep_tick): a hardware PWM tone only sounds while a PCM stream is already
 * clocking the codec DAC, so it cannot stand alone on this target.  These
 * platform hooks are therefore no-ops here -- the stock codec2 player would
 * drive them, but the HD2 build links the ADPCM player, which does not. */
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

/* --- GPS (UART2, polled) ------------------------------------------------- */
const struct gpsDevice *platform_initGps()
{
    return gps_HD2_init();
}
