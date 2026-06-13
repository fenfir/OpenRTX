/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Import the factory (Ailunce/Dahua) channel database into the OpenRTX
 * codeplug: read each vendor channel slot via the vendor-records accessor
 * (hd2_cps_records.h), translate it to an OpenRTX channel_t, and insert it
 * into the OpenRTX codeplug (cps_io.h / cps_io_HD2.c).  After this the
 * channels appear in the OpenRTX UI as normal, usable channels.
 *
 * Prerequisite: the vendor channel region must be present in W25Q flash at
 * the factory base (0x006F3000) -- e.g. pushed there with the diag 'd'
 * flash-write op from a captured .bin.  Triggered by the diag 'g' op.
 *
 * Scope of the translation (v1): name, RX/TX frequency, FM/DMR mode,
 * bandwidth, and TX power.  DMR color-code + timeslot are carried; the DMR
 * contact (a 24-bit ID in the vendor slot) maps to OpenRTX contact_index 0
 * for now -- contact-list import is a follow-up.  CTCSS/DCS tone translation
 * (vendor BCD encoding -> OpenRTX tone-table index) is also deferred; FM
 * channels import with tones disabled.
 */

#include "hd2_cps_records.h"
#include "interfaces/cps_io.h"
#include "core/cps.h"
#include "rtx/rtx.h"            /* opmode + bandwidth enums */
#include <string.h>
#include <stdint.h>

#define HD2_IMPORT_MAX_CH   512u   /* safety bound on the slot scan */

/*
 * Translate a vendor 2-byte sub-audio tone field (channel +0x24 RX / +0x26 TX)
 * into an OpenRTX fmInfo_t tone index + enable.
 *
 * Vendor encoding (little-endian 16-bit):
 *   0xFFFF                     = no tone
 *   high byte & 0xC0 == 0x80   = DCS normal   (low 12 bits = octal code)
 *   high byte & 0xC0 == 0xC0   = DCS inverted
 *   otherwise                  = CTCSS: the 16-bit word is the frequency in
 *                                0.1 Hz expressed as 4 BCD-in-hex nibbles,
 *                                e.g. bytes 70 06 -> 0x0670 -> 67.0 Hz.
 *
 * This is the CTCSS half: a recognised CTCSS frequency maps to its ctcss_tone[]
 * index and enables the tone; DCS and any out-of-table frequency import as
 * "no tone" (DCS support is a follow-up that needs the core tone model
 * extended -- stock fmInfo_t only indexes the CTCSS table).
 */
static void translate_tone(const uint8_t t[2], uint8_t *en, uint8_t *idx, uint8_t *type)
{
    *en = 0;
    *idx = 0;
    *type = TONE_CTCSS;

    uint16_t raw = (uint16_t) t[0] | ((uint16_t) t[1] << 8);
    if(raw == 0xFFFFu) return;            /* no tone */

    if((t[1] & 0x80u) != 0u)
    {
        /* DCS: high byte 0x80=normal, 0xC0=inverted; the octal code digits are
         * (t[1]&0xF) hundreds, (t[0]>>4) tens, (t[0]&0xF) ones. */
        uint16_t code = (uint16_t)((t[1] & 0x0Fu) * 64u
                                 + ((t[0] >> 4) & 0x0Fu) * 8u
                                 + ( t[0]       & 0x0Fu));
        for(uint8_t i = 0; i < DCS_CODE_NUM; ++i)
        {
            if(dcs_code[i] == code)
            {
                *en = 1;
                *idx = i;
                *type = ((t[1] & 0xC0u) == 0xC0u) ? TONE_DCS_I : TONE_DCS_N;
                return;
            }
        }
        return;                           /* unrecognised DCS code: disabled */
    }

    /* CTCSS: 4 BCD-in-hex nibbles -> frequency in 0.1 Hz units. */
    uint16_t freq = ((raw >> 12) & 0xFu) * 1000u
                  + ((raw >>  8) & 0xFu) *  100u
                  + ((raw >>  4) & 0xFu) *   10u
                  + ( raw        & 0xFu);

    for(uint8_t i = 0; i < CTCSS_FREQ_NUM; ++i)
    {
        if(ctcss_tone[i] == freq) { *en = 1; *idx = i; return; }
    }
    /* unrecognised CTCSS frequency: leave disabled */
}

static void translate_channel(const hd2_vendor_channel_t *v, channel_t *o)
{
    memset(o, 0, sizeof(*o));

    o->mode      = hd2_channel_is_dmr(v) ? OPMODE_DMR : OPMODE_FM;
    o->bandwidth = hd2_channel_is_wide(v) ? BW_25 : BW_12_5;
    o->rx_only   = 0;

    /* Vendor power enum (opt21 bits 3:2) -> OpenRTX power in mW. */
    uint8_t pw = (v->opt21 & HD2_CH_O21_POWER_MASK) >> HD2_CH_O21_POWER_SHIFT;
    switch(pw)
    {
        case HD2_PWR_HIGH: o->power = 5000; break;
        case HD2_PWR_MID:  o->power = 2500; break;
        case HD2_PWR_XLOW: o->power =  500; break;
        default:           o->power = 1000; break;   /* low */
    }

    o->rx_frequency = hd2_bcd4_to_hz(v->rx_freq);
    o->tx_frequency = hd2_bcd4_to_hz(v->tx_freq);

    /* Vendor name is 10 bytes, null/0xff-padded; OpenRTX name is CPS_STR_SIZE. */
    unsigned n = 0;
    for(; n < 10u && v->name[n] != 0 && (uint8_t)v->name[n] != 0xFFu; ++n)
        o->name[n] = v->name[n];
    o->name[n] = 0;

    if(o->mode == OPMODE_DMR)
    {
        uint8_t cc = (v->cc_ts_dmr & HD2_CH_O2A_CC_MASK) >> HD2_CH_O2A_CC_SHIFT;
        o->dmr.rxColorCode  = cc;
        o->dmr.txColorCode  = cc;
        o->dmr.dmr_timeslot = (v->cc_ts_dmr & HD2_CH_O2A_TS2) ? 2 : 1;
        o->dmr.contact_index = 0;        /* contact-list import is a follow-up */
    }
    else
    {
        uint8_t en, idx, type;
        translate_tone(v->rx_tone, &en, &idx, &type);
        o->fm.rxToneEn   = en;
        o->fm.rxTone     = idx;
        o->fm.rxToneType = type;
        translate_tone(v->tx_tone, &en, &idx, &type);
        o->fm.txToneEn   = en;
        o->fm.txTone     = idx;
        o->fm.txToneType = type;
    }
}

/*
 * Replace the OpenRTX codeplug with the factory channel list.
 * Returns the number of channels imported, or -1 on error.  *out_ch (if not
 * NULL) also receives the count.
 */
int hd2_import_vendor_codeplug(int *out_ch)
{
    if(cps_create(NULL) != 0) return -1;     /* fresh, empty OpenRTX codeplug */
    if(cps_open(NULL) != 0)  return -1;

    int n = 0;
    for(uint16_t i = 0; i < HD2_IMPORT_MAX_CH; ++i)
    {
        hd2_vendor_channel_t v;
        if(hd2_vendor_channel_read(i, &v) != 0) break;
        if(!hd2_vendor_channel_present(&v))   break;   /* slots are contiguous */

        channel_t o;
        translate_channel(&v, &o);
        if(cps_insertChannel(o, (uint16_t) n) != 0) break;
        n++;
    }

    if(out_ch) *out_ch = n;
    return n;
}
