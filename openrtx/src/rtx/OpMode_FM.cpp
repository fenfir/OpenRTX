/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/radio.h"
#include "rtx/OpMode_FM.hpp"
#include "rtx/rtx.h"

#if defined(PLATFORM_TTWRPLUS)
#include "drivers/baseband/AT1846S.h"
#endif

/**
 * \internal
 * On MD-UV3x0 radios the volume knob does not regulate the amplitude of the
 * analog signal towards the audio amplifier but it rather serves to provide a
 * digital value to be fed into the HR_C6000 lineout DAC gain. We thus have to
 * provide the helper function below to keep the real volume level consistent
 * with the knob position.
 */
#if defined(PLATFORM_TTWRPLUS)
void _setVolume()
{
    static uint8_t oldVolume = 0xFF;
    uint8_t volume = platform_getVolumeLevel();

    if(volume == oldVolume)
        return;

    // AT1846S volume control is 4 bit
    AT1846S::instance().setRxAudioGain(volume / 16, volume / 16);
    oldVolume = volume;
}
#endif

OpMode_FM::OpMode_FM() : rfSqlOpen(false), sqlOpen(false), enterRx(true)
{
}

OpMode_FM::~OpMode_FM()
{
}

/*
 * Weak default for the hardware RF-squelch hook: reports "no hardware RF
 * squelch", so update() below uses the radio_getRssi() threshold.  Targets
 * whose transceiver exposes its own squelch comparator (with on-chip
 * hysteresis) provide a strong override in their radio_*.c driver -- e.g. HD2
 * returns the AT1846S sq_cmp, which is far steadier than our raw RSSI read.
 */
extern "C" __attribute__((weak)) bool radio_checkRxRfSquelch(bool *open)
{
    (void) open;
    return false;
}

/*
 * Weak defaults for the FM TX-extras hooks (1750 burst / tail-elim / VOX).
 * Targets that support these override them in their radio driver; by default
 * the burst/tail are no-ops and VOX never triggers, so the TX path below is
 * plain PTT keying -- unchanged behaviour for radios without the extras.
 */
extern "C" __attribute__((weak)) void radio_fmToneBurst(void)         { }
extern "C" __attribute__((weak)) void radio_fmTailElim(void)          { }
extern "C" __attribute__((weak)) void radio_fmVoxArm(uint8_t level)   { (void) level; }
extern "C" __attribute__((weak)) bool radio_fmVoxDetected(void)       { return false; }

void OpMode_FM::enable()
{
    // When starting, close squelch and prepare for entering in RX mode.
    rfSqlOpen = false;
    sqlOpen   = false;
    enterRx   = true;
}

void OpMode_FM::disable()
{
    // Clean shutdown.
    platform_ledOff(GREEN);
    platform_ledOff(RED);
    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    radio_disableRtx();
    rfSqlOpen = false;
    sqlOpen   = false;
    enterRx   = false;
}

void OpMode_FM::update(rtxStatus_t *const status, const bool newCfg)
{
    (void) newCfg;

    #if defined(PLATFORM_TTWRPLUS)
    // Set output volume by changing the HR_C6000 DAC gain
    _setVolume();
    #endif

    // RX logic
    if(status->opStatus == RX)
    {
        // RF squelch mechanism.  Prefer the transceiver's own hardware RF
        // squelch (a steady on-chip RSSI+noise decision) when the device
        // provides one; otherwise fall back to thresholding the filtered RSSI.
        bool hwSqlOpen;
        if(radio_checkRxRfSquelch(&hwSqlOpen))
        {
            rfSqlOpen = hwSqlOpen;
        }
        else
        {
            // This turns squelch (0 to 15) into RSSI (-127.0dbm to -61dbm)
            rssi_t squelch = -127 + (status->sqlLevel * 66) / 15;
            rssi_t rssi    = rtx_getRssi();

            // Provide hysteresis: only change state when the RSSI moves more
            // than 4 dBm beyond the squelch setting (an 8 dBm open/close
            // window).  Widened from +-1 dBm (2026-06-10): a debug-cable ground
            // loop bounces the RSSI several dB, and the narrow +-1 window let
            // rfSqlOpen flicker, which thrashed the UI standby/backlight on
            // every status tick (the "RSSI haywire -> screen flips -> lockup"
            // path).  8 dBm absorbs the bench noise without hurting real
            // FM-voice squelch behaviour.
            if((rfSqlOpen == false) && (rssi > (squelch + 4))) rfSqlOpen = true;
            if((rfSqlOpen == true)  && (rssi < (squelch - 4))) rfSqlOpen = false;
        }

        // Local flags for current RF and tone squelch status
        bool rfSql   = ((status->rxToneEn == 0) && (rfSqlOpen == true));
        bool toneSql = ((status->rxToneEn == 1) && radio_checkRxDigitalSquelch());

        // Audio control
        if((sqlOpen == false) && (rfSql || toneSql))
        {
            rxAudioPath = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
            if(rxAudioPath > 0) sqlOpen = true;
        }

        if((sqlOpen == true) && (rfSql == false) && (toneSql == false))
        {
            audioPath_release(rxAudioPath);
            sqlOpen = false;
        }
    }
    else if((status->opStatus == OFF) && enterRx)
    {
        radio_disableRtx();

        radio_enableRx();
        status->opStatus = RX;
        enterRx = false;

        // (Re-)arm the VOX detector -- entering RX clears it.  No-op on radios
        // without VOX (weak default) or when status->vox == 0.
        radio_fmVoxArm(status->vox);
    }

    // TX logic: hardware PTT or VOX keying, with an optional 1750 Hz key-up
    // burst and CTCSS/DCS tail elimination on dekey.  (txIsVox/voxHang are
    // function-local statics -- OpMode_FM is a singleton.)
    static bool     txIsVox = false;        // current key started by VOX, not PTT
    static uint32_t voxHang = 0;            // VOX hangtime countdown (update ticks)
    const  uint32_t VOX_HANG_TICKS = 30u;   // ~0.9 s @ 33 Hz

    bool ptt = platform_getPttStatus();

    // Key entry: PTT always wins; VOX only while listening with squelch closed
    // (don't key over an incoming signal or self-trigger off speaker audio).
    bool keyByPtt = ptt && (status->txDisable == 0);
    bool keyByVox = (!keyByPtt) && (status->txDisable == 0) && (status->vox != 0)
                    && (status->opStatus == RX) && (sqlOpen == false)
                    && radio_fmVoxDetected();

    if((keyByPtt || keyByVox) && (status->opStatus != TX))
    {
        audioPath_release(rxAudioPath);
        radio_disableRtx();

        txAudioPath = audioPath_request(SOURCE_MIC, SINK_RTX, PRIO_TX);
        radio_enableTx();
        status->opStatus = TX;

        txIsVox = (keyByVox && !keyByPtt);
        voxHang = VOX_HANG_TICKS;

        if(status->toneBurst1750) radio_fmToneBurst();   // blocking ~0.75 s
    }

    if(status->opStatus == TX)
    {
        // A hardware PTT during a VOX key converts it to a held PTT key.
        if(txIsVox && ptt) txIsVox = false;

        // VOX-keyed: hold while speech continues, then run the hangtime.
        if(txIsVox)
        {
            if(radio_fmVoxDetected()) voxHang = VOX_HANG_TICKS;
            else if(voxHang > 0u)     voxHang--;
        }

        bool drop = txIsVox ? (voxHang == 0u) : (!ptt);
        if(drop)
        {
            // CTCSS/DCS tail-elimination reverse burst before the real dekey.
            if(status->tailElim && status->txToneEn) radio_fmTailElim();  // blocking

            audioPath_release(txAudioPath);
            radio_disableRtx();

            status->opStatus = OFF;
            enterRx = true;
            sqlOpen = false;  // Force squelch to be redetected.
            txIsVox = false;
        }
    }

    // Led control logic
    switch(status->opStatus)
    {
        case RX:
            if(radio_checkRxDigitalSquelch())
            {
                platform_ledOn(GREEN);  // Red + green LEDs ("orange"): tone squelch open
                platform_ledOn(RED);
            }
            else if(rfSqlOpen)
            {
                platform_ledOn(GREEN);  // Green LED only: RF squelch open
                platform_ledOff(RED);
            }
            else
            {
                platform_ledOff(GREEN);
                platform_ledOff(RED);
            }

            break;

        case TX:
            platform_ledOff(GREEN);
            platform_ledOn(RED);
            break;

        default:
            platform_ledOff(GREEN);
            platform_ledOff(RED);
            break;
    }

    // Sleep thread for 30ms for 33Hz update rate
    sleepFor(0u, 30u);
}

bool OpMode_FM::rxSquelchOpen()
{
    return sqlOpen;
}
