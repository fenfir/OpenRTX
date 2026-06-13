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
        o->fm.rxToneEn = 0;              /* tone translation deferred */
        o->fm.txToneEn = 0;
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
