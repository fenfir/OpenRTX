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

    /* Wait for the PCM handshake (socsys 0x88 bit31 = standby_lo clear): this is
     * the codec-DAC standby-EXIT handshake. If it times out, the DAC never
     * leaves standby -> silent even though the byte-regs read correct. */
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

    /* NOTE: the codec DAC plays CPU PCM without any AT1846S bring-up -- the
     * transceiver only feeds the modem RECEIVE path, which playback never uses.
     * (The earlier "codec needs at1846s.init() first" claim was a red herring
     * from the PTB17-silent era; PTB17 routing was the real fix.) */

    /* Full vendor hd2_modem_fm_boot_init: the on-chip codec-DAC/modem is a
     * DesignWare peripheral whose functional FSM stays DEAD until the ordered
     * clock-gate + power + reset sequence runs (project memory
     * hd2-dw-peripheral-clock-gate: clk_init_pll alone is NOT enough). Ordered:
     * REG2C clock-gate, SYS_SOFT_RSTN=0x1d0 reset pulse, codec init on the
     * freshly-reset audio block, DAC power/bias, then the datapath. */
    SOCSYS_REG2C = 0xfff0fffcu;
    SOCSYS_SYS_SOFT_RSTN = 0x000001d0u;

    hd2_codec_audio_init();

    /* Pin DIPLEX2 to the proven PCM-PLAYBACK state (bits 28/29 SET). The proven
     * app's playback path (radio_HD2.cpp thin hd2_audio_out_warm) never touches
     * DIPLEX2; at play time it reads 0x3ff80003 because the LCD/keypad scan
     * maintains it. The DIPLEX2 &= 0xCFFFFFFF clear only exists in the proven
     * FM-RX *boot* path (run seconds earlier), so it's long overwritten by the
     * time a tone plays. Doing that clear HERE, right before playing, raced the
     * LCD and left 28/29 clear during the tone -> silent. Write the LCD value
     * outright so playback is deterministic regardless of LCD-thread timing. */
    SOCSYS_IO_DIPLEX2 = HD2_DIPLEX2_LCD_I80;

    /* AF-receive DAC bias (manual §8.2.3.1): power up DAC channels B+C. */
    DAC_MCU_PD_MODE_EN = 0x00000001u;
    DAC_MCU_DATA_A = 0x00000000u;
    DAC_MCU_DATA_B = 0x00000000u;
    DAC_MCU_DATA_C = 0x00000000u;
    DAC_MCU_PD_CTRL = 0x00000001u;
    DAC_MCU_DATA_C = 0x000006e2u;

    SOCSYS_DAC_CONTROL = 0x8000001fu;  /* bit5 pwda clear -> DAC power-on */
    SOCSYS_ADC_CONTROL = 0x000041c3u;  /* modem ADC latch */
    SOCSYS_VOICE_PATH = 0x00000002u;
    SOCSYS_PCM_MODE = 0x00000000u;
    SOCSYS_LINEOUT_CTRL = 0x00000003u;
    SOCSYS_WORK_MODE = 0x0000006eu;
    SOCSYS_RF_MODE = 0x034c9060u;
    SOCSYS_RF_CONTROL = 0x00041f1au;
    SOCSYS_RF_IF_REG = 0x01e80000u;
    SOCSYS_THRESHOLD = 0x0978786fu;
    SOCSYS_SLOT_GUARD = 0x00000014u;
    SOCSYS_RX_IF_FREQ = 0x000bb800u;
    SOCSYS_RX_AGC = 0x000036b0u;
    SOCSYS_AF_GATE = 0x0001007fu;      /* Match proven hd2_modem_fm_boot_init /
                                        * pcm_tone warm exactly (0x1007f). bit16
                                        * reads back cleared, so an earlier
                                        * snapshot showed 0x7f and we wrote 0x7f
                                        * -- but the proven play path WRITES
                                        * 0x1007f; converge on it. */
    SOCSYS_MODEM_RXDP0 = 0x000000c4u;
    SOCSYS_MODEM_RXDP1 = 0x00000040u;

    warmed = true;
}
