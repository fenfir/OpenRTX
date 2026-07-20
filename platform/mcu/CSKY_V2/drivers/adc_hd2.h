/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Polling driver for the HR_C7000 on-chip ADC (Ailunce HD2 / CSKY V2).
 */

#ifndef ADC_HD2_H
#define ADC_HD2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One single-shot 10-bit conversion on `channel` (0..7; battery = ch2).
 * Returns the raw sample (0..1023), or 0 on timeout / invalid channel. */
uint16_t adc_hd2_sample(uint8_t channel);

/* One-time ADC controller reset + config (the vendor reset-release pulse the
 * FSM needs; without it DATA reads 0).  adc_hd2_sample() calls it lazily. */
void adc_hd2_init(void);

/* Battery-pack channel (ch2) with the vendor "(raw << 2)" pre-scale and a
 * 16-sample sliding average -- the value the voltage math expects. */
uint16_t adc_hd2_battery_raw_avg(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_HD2_H */
