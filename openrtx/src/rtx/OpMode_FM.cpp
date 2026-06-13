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
        // RF squelch mechanism
        // This turns squelch (0 to 15) into RSSI (-127.0dbm to -61dbm)
        rssi_t squelch = -127 + (status->sqlLevel * 66) / 15;
        rssi_t rssi    = rtx_getRssi();

        // Provide hysteresis: only change state when the RSSI moves more than
        // 4 dBm beyond the squelch setting (an 8 dBm open/close window).
        // Widened from +-1 dBm (2026-06-10): a debug-cable ground loop bounces
        // the RSSI several dB, and the narrow +-1 window let rfSqlOpen flicker,
        // which thrashed the UI standby/backlight on every status tick (the
        // "RSSI haywire -> screen flips -> lockup" path).  8 dBm absorbs the
        // bench noise without hurting real FM-voice squelch behaviour.
        if((rfSqlOpen == false) && (rssi > (squelch + 4))) rfSqlOpen = true;
        if((rfSqlOpen == true)  && (rssi < (squelch - 4))) rfSqlOpen = false;

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
    }

    // TX logic
    if(platform_getPttStatus() && (status->opStatus != TX) &&
                                  (status->txDisable == 0))
    {
        audioPath_release(rxAudioPath);
        radio_disableRtx();

        txAudioPath = audioPath_request(SOURCE_MIC, SINK_RTX, PRIO_TX);
        radio_enableTx();

        status->opStatus = TX;
    }

    if(!platform_getPttStatus() && (status->opStatus == TX))
    {
        audioPath_release(txAudioPath);
        radio_disableRtx();

        status->opStatus = OFF;
        enterRx = true;
        sqlOpen = false;  // Force squelch to be redetected.
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
