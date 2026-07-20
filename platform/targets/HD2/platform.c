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
#include "drivers/GPS/gps_HD2.h"
#include "hwconfig.h"

#include "drivers/audio/codec_HD2.h"

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
/* Beep = PWM channel 1 square wave mixed through the (warmed) codec lineout to
 * the speaker amp. Codec-free path would be silent -- the beep rides the codec,
 * so warm it first. platform_init already leaves DIPLEX0 audio-unmuted (bit18=0)
 * and does not need a read-modify-write here. */
void platform_beepStart(uint16_t freq)
{
    if (freq == 0u)
        return;

    hd2_audio_out_warm(); /* codec DAC -> lineout, once */

    /* Un-mute the PWM-audio path into the codec (DIPLEX0 bit18 clear). This is
     * the vendor's beep on/off gate -- clearing PWM enable alone does NOT
     * silence the tone. Full write off our known 0x60 DIPLEX0 baseline (upper
     * DIPLEX bits are write-only, so avoid a read-modify-write). */
    SOCSYS_IO_DIPLEX0 = 0x00000060u;

    /* Speaker amp: PTB4 LOW (unmute), PTB10 LOW (route to speaker),
     * PTB17 HIGH (gain -- the loudness enable). */
    GPIOB_DDR |= (SPKR_AMP_BIT | SPKR_GAIN_BIT | AUDIO_ROUTE_BIT);
    GPIOB_DR &= ~(SPKR_AMP_BIT | AUDIO_ROUTE_BIT);
    GPIOB_DR |= SPKR_GAIN_BIT;

    /* PWM ch1 tone, 50% duty (same channel-start order as the backlight ch0). */
    volatile uint32_t *p = PWM_CH1_BASE;
    p[0] &= ~1u;
    p[0] &= ~4u;
    p[0] &= ~8u;
    p[0] &= ~0x30u;
    p[0] |= 0x20u;
    p[0] |= 0x100u;
    p[0] |= 0x200u;
    p[1] = 5;
    p[2] = PWM_TIMER_HZ / freq;
    p[3] = p[2] / 2u;
    p[0] |= 4u;
    p[0] |= 1u;
}

void platform_beepStop()
{
    volatile uint32_t *p = PWM_CH1_BASE;
    p[0] &= ~1u;
    p[0] |= 2u;

    /* Mute the PWM-audio path into the codec (DIPLEX0 bit18 set) -- the vendor's
     * beep-off gate, and what actually silences the tone. Full write off the
     * 0x60 baseline. */
    SOCSYS_IO_DIPLEX0 = 0x00000060u | DIPLEX0_AUDIO_MUTE;

    /* Re-mute the amp (PTB4 HIGH, PTB17 LOW). */
    GPIOB_DR |= SPKR_AMP_BIT;
    GPIOB_DR &= ~SPKR_GAIN_BIT;
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
