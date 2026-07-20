/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone HR_C7000 codec audio-OUTPUT bring-up for the Ailunce HD2.
 *
 * This is the codec DAC -> LINE2OUT warm-up ONLY: the codec byte-register init
 * (MC interface @0x16000900) plus the SOCSYS audio gate. It has NO AT1846S / RF
 * tune and NO modem-RX servicing loop -- it is the minimal, radio-free subset
 * needed to make the codec DAC (and the PWM-ch1 beep that mixes through the
 * codec lineout, and the SAHB PCM bridge) audible. Idempotent; call lazily.
 *
 * The register sequence is the vendor V2.1.3 audio-out path, HW-verified
 * (extracted from the proven radio_HD2.cpp::hd2_audio_out_warm so audio links
 * without the radio stack).
 */

#include "codec_HD2.h"
#include "hd2_regs.h"
#include <stdint.h>
#include <stdbool.h>

extern void delayMs(unsigned int ms);

#define CB(off) CODEC_BYTE(off)

/* Codec chip bring-up over the MC (modem-control) byte interface. */
static void hd2_codec_audio_init(void)
{
    /* PCM block reset pulse + codec soft-reset (vendor exact). */
    CB(0xd2) |= 0x03u;
    SOCSYS_SYS_SOFT_RSTN &= 0xffffffefu; /* clear bit4 = codec reset */
    delayMs(2);
    CB(0xd2) &= ~0x01u;
    delayMs(100);
    CB(0xd2) &= ~0x02u;
    CB(0xd3) = 0x40u;
    CB(0xcb) = 0x00u;
    CB(0xcc) |= 0x40u;
    CB(0xc9) = 0xc0u; /* ADC iface: master, enable, parallel, 8k */
    CB(0xcf) &= ~0x10u;
    delayMs(100);
    CB(0xcf) &= ~0x80u;
    CB(0xc8) = 0xc0u; /* DAC iface: master, enable, parallel, 8k */
    CB(0xcd) &= ~0x10u;

    /* Wait for the PCM handshake (socsys 0x88 bit31 clear), bounded. */
    for (uint32_t g = 0; g < 200u; ++g) {
        if ((SOCSYS_PCM_HANDSHAKE & 0x80000000u) == 0u)
            break;
        delayMs(10);
    }

    CB(0xcd) &= ~0x80u;
    delayMs(100);
    CB(0xdf) = CODEC_DACL_GAIN;
    CB(0xe5) = 0x8bu;
    SOCSYS_PCM_HANDSHAKE = 1u;

    /* Analog-out tail. */
    CB(0xc8) = 0xc0u;
    CB(0xcd) = 0x20u;
    CB(0xe5) = 0x8bu;
    CB(0xdf) = CODEC_DACL_GAIN;
    SOCSYS_PCM_MODE |= 0x02u;
}

void hd2_audio_out_warm(void)
{
    static bool warmed = false;
    if (warmed)
        return;

    hd2_codec_audio_init();

    /* SOCSYS audio gate for the codec-DAC -> lineout leg. These are opaque
     * HW-verified warm values; MODEM_RXDP0/1 are poked as part of the audio
     * gate (not radio protocol -- no RF is keyed). */
    SOCSYS_DAC_CONTROL = 0x8000001fu;
    SOCSYS_ADC_CONTROL = 0x000041c3u;
    SOCSYS_VOICE_PATH = 0x00000002u;   /* direct codec DAC (PCM bridge OFF) */
    SOCSYS_PCM_MODE = 0x00000000u;
    SOCSYS_LINEOUT_CTRL = 0x00000003u; /* both lineouts; speaker is LINE2OUT */
    SOCSYS_WORK_MODE = 0x0000006eu;    /* FM-analog audio gate */
    SOCSYS_AF_GATE = 0x0001007fu;
    SOCSYS_MODEM_RXDP0 = 0x000000c4u;
    SOCSYS_MODEM_RXDP1 = 0x00000040u;

    warmed = true;
}
