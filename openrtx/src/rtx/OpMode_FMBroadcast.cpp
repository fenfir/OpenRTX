/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/OpMode_FMBroadcast.hpp"
#include "interfaces/tuner.h"
#include "interfaces/delays.h"

/*
 * Weak default tuner HAL: "no broadcast tuner present".  Targets with a
 * broadcast-FM tuner (dedicated chip or the main transceiver's broadcast mode)
 * provide strong overrides in their driver -- e.g. HD2 maps these onto the
 * RDA5802E.  With the defaults, OpMode_FMBroadcast is an inert mode.
 */
extern "C" __attribute__((weak)) void     tuner_init(void)              { }
extern "C" __attribute__((weak)) void     tuner_powerUp(void)           { }
extern "C" __attribute__((weak)) void     tuner_powerDown(void)         { }
extern "C" __attribute__((weak)) void     tuner_tune(uint32_t)          { }
extern "C" __attribute__((weak)) uint8_t  tuner_rssi(void)              { return 0; }
extern "C" __attribute__((weak)) bool     tuner_getStatus(bool *tuned, uint16_t *channel)
{
    if(tuned)   *tuned   = false;
    if(channel) *channel = 0;
    return false;
}

OpMode_FMBroadcast::OpMode_FMBroadcast()
    : active(false), tuned(false), curFreq(0), rssi(0)
{
}

OpMode_FMBroadcast::~OpMode_FMBroadcast()
{
}

void OpMode_FMBroadcast::enable()
{
    // Power up the tuner + route its audio to the speaker.  curFreq=0 forces a
    // tune on the first update().
    tuner_powerUp();
    active  = true;
    tuned   = false;
    curFreq = 0;
    rssi    = 0;
}

void OpMode_FMBroadcast::disable()
{
    tuner_powerDown();
    active = false;
    tuned  = false;
}

void OpMode_FMBroadcast::update(rtxStatus_t *const status, const bool newCfg)
{
    (void) newCfg;

    // Re-tune when the UI changes the frequency (rxFrequency carries the
    // broadcast tune target in Hz; the tuner HAL takes kHz).  curFreq=0 after
    // enable() forces the first tune.
    if(status->rxFrequency != curFreq)
    {
        tuner_tune(status->rxFrequency / 1000u);
        curFreq = status->rxFrequency;
    }

    // Poll lock + signal level for the UI / squelch indication.
    uint16_t channel = 0;
    bool     locked  = false;
    rssi  = tuner_rssi();
    tuner_getStatus(&locked, &channel);
    tuned = locked;

    // Broadcast tuning is slow-moving; a relaxed poll keeps the shared tuner
    // I2C bus quiet (matches the proven 4 Hz worker rate).
    sleepFor(0u, 250u);
}
