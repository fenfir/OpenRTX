/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

#include "hd2_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Screen: 160x128 ST7735S TFT, RGB565, HW i8080 bus */
#define CONFIG_SCREEN_WIDTH 160
#define CONFIG_SCREEN_HEIGHT 128
#define CONFIG_PIX_FMT_RGB565

/* LCD backlight is a PWM-dimmed LED */
#define CONFIG_SCREEN_BRIGHTNESS

/* On-SoC real-time clock (32.768 kHz xtal, coin-cell backed) */
#define CONFIG_RTC

/* Battery: 2-cell Li-Ion */
#define CONFIG_BAT_LIION
#define CONFIG_BAT_NCELLS 2

#ifdef __cplusplus
}
#endif

#endif /* HWCONFIG_H */
