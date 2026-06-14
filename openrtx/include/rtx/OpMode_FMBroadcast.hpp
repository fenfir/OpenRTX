/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_FMBROADCAST_H
#define OPMODE_FMBROADCAST_H

#include "rtx/OpMode.hpp"

/**
 * Specialisation of the OpMode class for broadcast-FM receive.
 *
 * Drives the broadcast tuner via the device-agnostic tuner HAL (interfaces/
 * tuner.h): enable powers it up + routes audio, update re-tunes on a frequency
 * change and polls the lock/RSSI status, disable powers it down.  Receive-only
 * (no TX), and it does not use the transceiver -- on radios with a dedicated
 * tuner chip it is an entirely separate signal path; on radios that tune
 * broadcast via the main chip, the tuner HAL maps onto that.
 */
class OpMode_FMBroadcast : public OpMode
{
public:

    OpMode_FMBroadcast();
    ~OpMode_FMBroadcast();

    virtual void enable() override;
    virtual void disable() override;
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_FM_BCAST;
    }

    virtual bool rxSquelchOpen() override
    {
        return tuned;
    }

private:

    bool     active;        ///< Tuner powered up.
    bool     tuned;         ///< A station is locked.
    uint32_t curFreq;       ///< Last frequency tuned, in Hz (0 = none).
    uint8_t  rssi;          ///< Last RSSI reading.
};

#endif /* OPMODE_FMBROADCAST_H */
