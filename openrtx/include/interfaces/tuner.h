/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TUNER_H
#define TUNER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Device-agnostic broadcast-FM tuner HAL, driven by OpMode_FMBroadcast.
 *
 * Radios implement broadcast-FM receive in different ways -- a dedicated tuner
 * chip (e.g. the HD2's RDA5802E on its own I2C bus) or the main transceiver's
 * own broadcast mode (the AT1846S has one).  This interface abstracts that so
 * the broadcast OpMode is portable.  The audio routing to the speaker is owned
 * by the implementation (it is a board-level analog concern), so the OpMode
 * never touches the audio matrix for broadcast.
 *
 * The default (weak) implementation is a no-op / "no tuner", so targets without
 * broadcast-FM support link cleanly and OpMode_FMBroadcast simply does nothing.
 */

/**
 * \brief One-time tuner bring-up (bus + GPIO).  Idempotent.
 */
void tuner_init(void);

/**
 * \brief Power the tuner up and route its audio to the speaker.  Called on
 *        OpMode_FMBroadcast enable.
 */
void tuner_powerUp(void);

/**
 * \brief Power the tuner down and hand the audio path back.  Called on disable.
 */
void tuner_powerDown(void);

/**
 * \brief Tune to a broadcast frequency.
 * @param freq_khz: target frequency in kHz (e.g. 103600 for 103.6 MHz).
 */
void tuner_tune(uint32_t freq_khz);

/**
 * \brief Current received signal strength (implementation-scaled, 0 = none).
 */
uint8_t tuner_rssi(void);

/**
 * \brief Read tuner status.
 * @param tuned:   receives true when a station is locked (seek/tune complete).
 * @param channel: receives the tuner's read-back channel (implementation value).
 * @return true if status was read.
 */
bool tuner_getStatus(bool *tuned, uint16_t *channel);

#ifdef __cplusplus
}
#endif

#endif /* TUNER_H */
