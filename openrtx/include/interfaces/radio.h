/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>
#include "rtx/rtx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This file provides a common interface for the platform-dependent low-level
 * rtx driver. Top level application code normally does not have to call directly
 * the API functions provided here, since all the transceiver managment, comprised
 * the handling of digital protocols is done by the 'rtx' module.
 *
 * The radio functionalities are controlled by means of an rtxStatus_t data
 * structure containing all the parameters required to define a given operating
 * configuration for the RF stage such as TX and RX frequencies, ...
 * The data structure is internally accessed by each of the API functions and is
 * guaranteed that the access is performed in read only mode.
 */

/**
 * Initialise low-level radio transceiver.
 *
 * @param rtxState: pointer to an rtxStatus_t structure used to describe the
 * operating configuration of the radio module.
 */
void radio_init(const rtxStatus_t *rtxState);

/**
 * Shut down low-level radio transceiver.
 */
void radio_terminate();

/**
 * Set current operating mode.
 *
 * @param mode: new operating mode.
 */
void radio_setOpmode(const enum opmode mode);

/**
 * Check if digital squelch is opened, that is if a CTC/DCS code is being
 * detected.
 *
 * @return true if RX digital squelch is enabled and if the configured CTC/DCS
 * code is present alongside the carrier.
 */
bool radio_checkRxDigitalSquelch();

/**
 * Hardware RF-squelch (carrier-detect) hook.  Transceivers that compute their
 * own RSSI+noise squelch decision (with on-chip hysteresis) override this to
 * surface that decision, which is far steadier than thresholding radio_getRssi()
 * in OpMode_FM.  The default (weak) implementation reports "no hardware RF
 * squelch", so OpMode_FM falls back to the RSSI-threshold path unchanged.
 *
 * @param open: receives the hardware RF-squelch state (true = open / carrier
 *              present) when the device supports it.
 * @return true if the device provides a hardware RF squelch (\p open is valid);
 *         false to use the radio_getRssi() threshold fallback.
 */
bool radio_checkRxRfSquelch(bool *open);

/**
 * FM TX key-up tone burst (e.g. 1750 Hz repeater access).  BLOCKING: sounds the
 * burst for its fixed duration then stops, with the carrier already keyed.
 * Called from OpMode_FM on TX entry when rtxStatus.toneBurst1750 is set.  Weak
 * default is a no-op.
 */
void radio_fmToneBurst(void);

/**
 * FM CTCSS/DCS tail-elimination reverse burst on dekey.  BLOCKING: holds the
 * keyed carrier with the reverse-phase sub-audio for its fixed duration so the
 * far end drops carrier without a squelch tail; the caller dekeys afterwards.
 * Called from OpMode_FM before disabling TX when rtxStatus.tailElim is set
 * (and a TX tone is active).  Weak default is a no-op.
 */
void radio_fmTailElim(void);

/**
 * Arm/disarm the FM VOX detector for RX-side voice keying.
 * @param level: VOX sensitivity 1..5 (0 = disable).  Weak default is a no-op.
 */
void radio_fmVoxArm(uint8_t level);

/**
 * @return true while the VOX detector reports voice present.  Weak default
 *         returns false (no VOX).
 */
bool radio_fmVoxDetected(void);

/**
 * Enable AF output towards the speakers.
 */
void radio_enableAfOutput();

/**
 * Disable AF output towards the speakers.
 */
void radio_disableAfOutput();

/**
 * Enable the RX stage.
 */
void radio_enableRx();

/**
 * Enable the TX stage.
 */
void radio_enableTx();

/**
 * Disable both the RX and TX stages, as long as transmission of CTC/DCS code
 * and digital squelch.
 */
void radio_disableRtx();

/**
 * Update configuration of the radio module to match the one currently described
 * by the rtxStatus_t configuration data structure.
 * This function has to be called whenever the configuration data structure has
 * been updated, to ensure all the operating parameters of the radio driver are
 * correctly configured.
 */
void radio_updateConfiguration();

/**
 * Get the current RSSI level in dBm.
 *
 * @return RSSI level in dBm.
 */
rssi_t radio_getRssi();

/**
 * Get the current operating status of the radio module.
 *
 * @return current operating status.
 */
enum opstatus radio_getStatus();

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
