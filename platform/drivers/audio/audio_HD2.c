/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenRTX audio.h routing driver for the Ailunce HD2 (HR_C7000), radio-free.
 *
 * Implements the audio-path HAL scoped to CPU playback: the only wired route
 * is SOURCE_MCU -> SINK_SPK (beep / voice-prompt / PCM), through the codec DAC
 * -> LINE2OUT -> speaker amp. There is NO radio: SINK_RTX / SOURCE_MIC / RTX
 * routing is intentionally absent (no AT1846S, no hd2_router). This file owns
 * only the board-level speaker-amp GPIO (PTB4/PTB10/PTB17); the codec bring-up
 * lives in codec_HD2.c (hd2_audio_out_warm).
 */

#include "interfaces/audio.h"
#include "drivers/audio/codec_HD2.h"
#include "hd2_regs.h"

#define PATH(x, y) (((x) << 4) | (y))

/* Path compatibility matrix (source*3 + sink). Single shared codec DAC ->
 * speaker; conservative. Same layout as the reference targets. */
static const uint8_t pathCompatibilityMatrix[9][9] = {
    //         M-S M-R M-M R-S R-R R-M C-S C-R C-M
    /* MIC-SPK */ {0, 0, 0, 0, 1, 1, 0, 1, 1},
    /* MIC-RTX */ {0, 0, 0, 1, 0, 1, 1, 0, 1},
    /* MIC-MCU */ {0, 0, 0, 1, 1, 0, 1, 1, 0},
    /* RTX-SPK */ {0, 1, 1, 0, 0, 0, 0, 1, 1},
    /* RTX-RTX */ {1, 0, 1, 0, 0, 0, 1, 0, 1},
    /* RTX-MCU */ {1, 1, 0, 0, 0, 0, 1, 1, 0},
    /* MCU-SPK */ {0, 1, 1, 0, 1, 1, 0, 0, 0},
    /* MCU-RTX */ {1, 0, 1, 1, 0, 1, 0, 0, 0},
    /* MCU-MCU */ {1, 1, 0, 1, 1, 0, 0, 0, 0}};

/* CPU->codec-DAC PCM stream driver (outputStream_HD2.cpp). 8 kHz mono s16. */
extern const struct audioDriver hd2_pcm_audio_driver;

const struct audioDevice outputDevices[] = {
    {NULL, 0, 0, SINK_MCU},
    {NULL, 0, 0, SINK_RTX}, /* no radio sink */
    {&hd2_pcm_audio_driver, NULL, 0, SINK_SPK},
};

const struct audioDevice inputDevices[] = {
    {NULL, 0, 0, SINK_MCU},
    {NULL, 0, 0, SINK_RTX},
    {NULL, 0, 0, SINK_SPK},
};

/* --- board-level speaker-amp helpers (plain RMW: only the UI thread + the
 * platform beep touch these bits in this radio-free build). ---------------- */
/* PTB17 (SPKR_GAIN_BIT) is an analog PATH SELECT, not a gain: LOW = codec DAC
 * lineout, HIGH = AT1846S analog demod. Keep it LOW for all MCU/codec playback
 * (beep / voice prompt / PCM); driving it HIGH routes the amp to the AT1846S
 * input instead, so the codec DAC is silent. See docs openrtx-audio (PTB17). */
static inline void spkr_amp_mute(void)
{
    GPIOB_DR |= SPKR_AMP_BIT;   /* PTB4  HIGH = muted        */
    GPIOB_DR &= ~SPKR_GAIN_BIT; /* PTB17 LOW  = codec path   */
}

static inline void spkr_amp_unmute(void)
{
    GPIOB_DR &= ~SPKR_AMP_BIT;  /* PTB4  LOW = on            */
    GPIOB_DR &= ~SPKR_GAIN_BIT; /* PTB17 LOW = select codec DAC (NOT AT1846S) */
}

static inline void rx_route_on(void)
{
    GPIOB_DR &= ~AUDIO_ROUTE_BIT; /* PTB10 LOW = routed to speaker */
}

void audio_init()
{
    GPIOB_DDR |= (SPKR_AMP_BIT | SPKR_GAIN_BIT | AUDIO_ROUTE_BIT);
    spkr_amp_mute();
}

void audio_terminate()
{
    spkr_amp_mute();
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    switch(PATH(source, sink))
    {
        case PATH(SOURCE_MCU, SINK_SPK):
            /* MCU playback (beep / voice prompt / PCM): the codec DAC ->
             * lineout must be warm, then route + unmute the speaker amp. */
            hd2_audio_out_warm();
            rx_route_on();
            spkr_amp_unmute();
            break;

        default:
            /* No radio paths (RTX/MIC) in this build; other routes are no-ops. */
            break;
    }
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    switch(PATH(source, sink))
    {
        case PATH(SOURCE_MCU, SINK_SPK):
            spkr_amp_mute();
            break;

        default:
            break;
    }
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    uint8_t p1Index = (p1Source * 3) + p1Sink;
    uint8_t p2Index = (p2Source * 3) + p2Sink;
    return pathCompatibilityMatrix[p1Index][p2Index] == 1;
}
