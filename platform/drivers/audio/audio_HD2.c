/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenRTX audio.h routing driver for the Ailunce HD2 (HR_C7000).
 *
 * Implements the standard OpenRTX audio path matrix (sources {MIC,RTX,MCU} ->
 * sinks {SPK,RTX,MCU}), modeled on platform/drivers/audio/audio_GDx.c.  It is
 * the single owner of the HD2's *board-level* audio routing: the GPIOB
 * speaker-amp (PTB4) + gain (PTB17) + RX-audio route (PTB10) lines that
 * radio_HD2.cpp deliberately left "to the audio agent", plus the codec /
 * socsys audio-gate bring-up.
 *
 * Division of labour (one source of truth for the HW-verified register
 * values):
 *   - The codec init + socsys audio-gate sequence (DAC/ADC/VOICE_PATH/LINEOUT/
 *     WORK_MODE/AF_GATE/...) was captured LIVE from a vendor unit playing FM and
 *     lives in radio_HD2.cpp::hd2_audio_out_warm().  We CALL that rather than
 *     re-hardcoding the magic constants here.
 *   - The AT1846S RX-audio chip-side mute (reg 0x30 bit7) is released via the
 *     radio.h hook radio_enableAfOutput()/disableAfOutput().
 *   - This file owns only the GPIO twiddles.
 *
 * NOTE (FM bring-up build): there is NO MCU->codec-DAC PCM playback path in
 * this build (outputStream_HD2.cpp is not linked), so the SINK_SPK output
 * device carries a NULL driver -- the endpoint enum is kept, but no PCM stream
 * driver is wired.  The analog FM-RX path (SOURCE_RTX -> SINK_SPK) is pure GPIO
 * and is fully preserved.
 *
 * TX (MIC->RTX) is intentionally not keyed here.
 */

#include "interfaces/audio.h"
#include "interfaces/radio.h"
#include "drivers/GPIO/gpio_hrc7000.h"

/* Codec + socsys audio-gate warm-up (HW-verified constants live in
 * radio_HD2.cpp).  Idempotent: a static latch makes repeat calls free. */
extern void hd2_audio_out_warm(void);

/* RF-freeze flag (radio_HD2.cpp).  While set, the audio matrix must not
 * rewrite the amp/route GPIOs that a host-side experiment may be holding --
 * audio_connect/disconnect become no-ops. */
extern volatile uint32_t g_rf_freeze;

/* Deferred AT1846S RX-AF mute (radio_HD2.cpp): set here on MCU->SPK connect so
 * the rtx thread mutes the live analog FM-RX audio out of the shared speaker
 * node (else it buries quiet PCM as hiss).  A plain write -- the actual AT1846S
 * I2C is done by the rtx thread, never here (UI-thread I2C wedges the bus). */
extern volatile uint8_t g_rx_af_mute_req;

#define PATH(x, y) (((x) << 4) | (y))

/*
 * Path compatibility matrix -- can two paths be open simultaneously?
 * Indexed by (source * 3 + sink) for each of the two paths.
 *
 * Row/col order: MIC-SPK MIC-RTX MIC-MCU RTX-SPK RTX-RTX RTX-MCU MCU-SPK MCU-RTX MCU-MCU
 */
static const uint8_t pathCompatibilityMatrix[9][9] = {
    //         M-S M-R M-M R-S R-R R-M C-S C-R C-M
    /* MIC-SPK */ { 0, 0, 0, 0, 1, 1, 0, 1, 1 },
    /* MIC-RTX */ { 0, 0, 0, 1, 0, 1, 1, 0, 1 },
    /* MIC-MCU */ { 0, 0, 0, 1, 1, 0, 1, 1, 0 },
    /* RTX-SPK */ { 0, 1, 1, 0, 0, 0, 0, 1, 1 },
    /* RTX-RTX */ { 1, 0, 1, 0, 0, 0, 1, 0, 1 },
    /* RTX-MCU */ { 1, 1, 0, 0, 0, 0, 1, 1, 0 },
    /* MCU-SPK */ { 0, 1, 1, 0, 1, 1, 0, 0, 0 },
    /* MCU-RTX */ { 1, 0, 1, 1, 0, 1, 0, 0, 0 },
    /* MCU-MCU */ { 1, 1, 0, 1, 1, 0, 0, 0, 0 }
};

/* No MCU->codec-DAC PCM stream driver in this FM build (outputStream_HD2.cpp
 * is not linked): SINK_SPK carries a NULL driver.  The endpoint enum is kept so
 * the audio_path core still resolves the sink. */
const struct audioDevice outputDevices[] = {
    { NULL, 0, 0, SINK_MCU },
    { NULL, 0, 0, SINK_RTX },
    { NULL, 0, 0, SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { NULL, 0, 0, SINK_MCU },
    { NULL, 0, 0, SINK_RTX },
    { NULL, 0, 0, SINK_SPK },
};

/* --- board-level helpers ------------------------------------------------ */

static inline void spkr_amp_mute(void)
{
    gpio_setPin(GPIOB, SPKR_AMP_PIN);    /* PTB4  HIGH = muted    */
    gpio_clearPin(GPIOB, SPKR_GAIN_PIN); /* PTB17 LOW  = low gain */
}

static inline void spkr_amp_unmute(void)
{
    gpio_clearPin(GPIOB, SPKR_AMP_PIN); /* PTB4  LOW  = on       */
    gpio_setPin(GPIOB, SPKR_GAIN_PIN);  /* PTB17 HIGH = full gain
                                            * (without it ALL speaker
                                            * audio is barely audible
                                            * -- see registers.h) */
}

static inline void rx_route_on(void)
{
    gpio_clearPin(GPIOB, AUDIO_ROUTE_PIN);
} /* PTB10 LOW = routed */

static inline void rx_route_off(void)
{
    gpio_setPin(GPIOB, AUDIO_ROUTE_PIN);
} /* PTB10 HIGH = un-routed (close the route gate) */

void audio_init()
{
    /* Drive the amp + gain + route lines as outputs; start with the speaker
     * muted (PTB4 HIGH, PTB17 LOW), matching the vendor's tuning state. */
    gpio_setMode(GPIOB, SPKR_AMP_PIN, OUTPUT);
    gpio_setMode(GPIOB, SPKR_GAIN_PIN, OUTPUT);
    gpio_setMode(GPIOB, AUDIO_ROUTE_PIN, OUTPUT);
    spkr_amp_mute();
    rx_route_off(); /* start with the RX-audio route closed (PTB10 HIGH) */
}

void audio_terminate()
{
    spkr_amp_mute();
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    if (g_rf_freeze != 0u) /* rf_freeze: no audio-GPIO rewrites */
        return;

    switch (PATH(source, sink)) {
        case PATH(SOURCE_RTX, SINK_SPK):
            /* FM RX audio -> speaker.  PURE-GPIO gate (must be trivial: this
             * fires on every squelch crossing).  The heavy AT1846S AF-DSP
             * config + chip-side unmute already ran once in radio_enableRx;
             * here we only route the analog AF (PTB10) and unmute the speaker
             * amp (PTB4). */
            rx_route_on();
            spkr_amp_unmute();
            break;

        case PATH(SOURCE_MCU, SINK_SPK):
            /* Beep / voice-prompt playback from the MCU (mixes through the
             * codec DAC -> lineout -> speaker, so the codec must be warm). */
            hd2_audio_out_warm();
            g_rx_af_mute_req =
                1u; /* rtx thread mutes the live analog FM-RX audio */
            rx_route_on();
            spkr_amp_unmute();
            break;

        case PATH(SOURCE_MIC, SINK_RTX):
            /* TX (mic -> transceiver).  Keying is owned by radio_enableTx;
             * left as an explicit no-op here. */
            break;

        default:
            break;
    }
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    if (g_rf_freeze != 0u) /* rf_freeze: no audio-GPIO rewrites */
        return;

    switch (PATH(source, sink)) {
        case PATH(SOURCE_RTX, SINK_SPK):
            /* Squelch gate: mute the amp AND close the RX-audio route (PTB10).
             * The route is opened on connect but was never closed, so it latched
             * open on the first RX and left the AT1846S demod noise floor a
             * permanent path to the amp (a faint squeal that appeared on the
             * first audio event and persisted).  The AT1846S AF config +
             * chip-side unmute stay in place so re-open is still a GPIO toggle. */
            spkr_amp_mute();
            rx_route_off();
            break;

        case PATH(SOURCE_MCU, SINK_SPK):
            spkr_amp_mute();
            rx_route_off();        /* close the RX-audio route (PTB10) */
            g_rx_af_mute_req = 0u; /* rtx thread restores the RX AF output */
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
