/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Polling driver for the HR_C7000 on-chip ADC (Ailunce HD2 / CSKY V2 ck803s),
 * base 0x140d0000 (manual 4.13).  Single-shot 10-bit conversions, busy-polled
 * (no RTOS/semaphore).  The battery pack is on channel 2 -> ADC_DATA_CD[9:0].
 *
 * The one-time adc_hd2_init() reset-release pulse is essential: without it the
 * conversion FSM stays held and DATA reads 0 (live-verified 2026-06-01; with
 * it ch2 reads a stable ~0x350).
 */

#include "drivers/adc_hd2.h"
#include "hd2_regs.h"

#define ADC_STATE_BUSY 0x01u    /* ADC_CTRL_BUSY              */
#define ADC_STATE_FSM 0x3eu     /* SAMP_FSM_STATE (bits[5:1]) */
#define ADC_SAMPLE_MASK 0x3ffu  /* 10-bit result             */
#define ADC_GUARD_ITERS 200000u /* bounded busy-wait guard   */

#define ADC_BATT_CHANNEL 2u
#define ADC_AVG_WINDOW 16u

static int g_adc_inited = 0;

/* Wait for BUSY + SAMP_FSM clear.  Returns 1 on idle, 0 on timeout. */
static int adc_wait_idle(void)
{
    for (uint32_t guard = 0; guard < ADC_GUARD_ITERS; ++guard) {
        uint32_t st = ADC_CTRL_STATE;
        if ((st & (ADC_STATE_BUSY | ADC_STATE_FSM)) == 0u)
            return 1;
    }
    return 0;
}

void adc_hd2_init(void)
{
    SOCSYS_IO_DIPLEX2 &= 0xffc7ffffu; /* clear bits 19-21: ADC input pin-mux */
    ADC_CTRL = 0u;
    ADC_CTRL = 8u; /* PD_FORCE soft-reset (the missing step) */
    ADC_CTRL_STOP = 2u;
    (void)adc_wait_idle();
    ADC_CTRL = 0u; /* release reset -> normal operation */
    ADC_SEOC_TIME = 0xa20u;
    ADC_P2S_EN = 0u;
    ADC_INTR = 0u;
    ADC_CH_VLD = 0u;
    g_adc_inited = 1;
}

uint16_t adc_hd2_sample(uint8_t channel)
{
    if (channel > 7u)
        return 0;
    if (!g_adc_inited)
        adc_hd2_init();
    if (!adc_wait_idle())
        return 0;

    ADC_CH_VLD = (1u << channel);
    ADC_START = 1u;

    if (!adc_wait_idle())
        return 0;

    uint32_t data;
    switch (channel >> 1) {
        case 0:
            data = ADC_DATA_AB;
            break; /* ch 0/1 */
        case 1:
            data = ADC_DATA_CD;
            break; /* ch 2/3 */
        case 2:
            data = ADC_DATA_EF;
            break; /* ch 4/5 */
        default:
            data = ADC_DATA_GH;
            break; /* ch 6/7 */
    }
    if (channel & 1u)
        data >>= 16;

    return (uint16_t)(data & ADC_SAMPLE_MASK);
}

uint16_t adc_hd2_battery_raw_avg(void)
{
    static uint16_t window[ADC_AVG_WINDOW];
    static int seeded = 0;

    uint16_t sample = (uint16_t)((adc_hd2_sample(ADC_BATT_CHANNEL) & 0x3fffu)
                                 << 2);

    if (!seeded) {
        for (uint32_t i = 0; i < ADC_AVG_WINDOW; ++i)
            window[i] = sample;
        seeded = 1;
    } else {
        for (uint32_t i = 0; i < ADC_AVG_WINDOW - 1u; ++i)
            window[i] = window[i + 1u];
        window[ADC_AVG_WINDOW - 1u] = sample;
    }

    uint32_t sum = 0;
    for (uint32_t i = 0; i < ADC_AVG_WINDOW; ++i)
        sum += window[i];

    return (uint16_t)((sum & 0x7ffffu) >> 4); /* sum / 16 */
}
