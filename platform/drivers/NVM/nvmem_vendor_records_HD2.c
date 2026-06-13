/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read-only accessor for the vendor factory channel/contact records
 * (hd2_cps_records.h) on the W25Q512 over HW SPI0.
 *
 * Records live in the vendor "channels" family-0x31 region.  Physical W25Q
 * byte offset = CPS block_index * 0x400 (validated in nvmem_HD2.c):
 *   priority contacts  block 0x1B84 -> 0x006E1000, 72 B stride, 14 / block
 *   channel slots      block 0x1BCC -> 0x006F3000, 176 B stride, 5 / block,
 *                      first slot at block offset 0x80
 * Within each 0x400 block the records pack from the block base (contacts) or
 * from +0x80 (channels); record N is in block (N / per-block), at the
 * in-block slot (N % per-block).
 *
 * READ-ONLY by design: this surfaces the factory database (e.g. for an
 * import-to-OpenRTX-codeplug feature).  The OpenRTX codeplug (cps_io_HD2.c)
 * owns all writes.  Flash primitives mirror the sibling NVM drivers (TODO:
 * factor a shared flash_w25q_HD2 driver -- four copies now).
 */

#include "hd2_cps_records.h"
#include "flash_w25q_HD2.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Vendor region geometry (see header + nvmem_HD2.c). */
#define VND_BLOCK_SIZE     0x400u
#define VND_CONTACT_BASE   0x006E1000u
#define VND_CONTACT_PERBLK 14u
#define VND_CHAN_BASE      0x006F3000u
#define VND_CHAN_PERBLK    5u
#define VND_CHAN_SLOT0     0x80u        /* first channel slot's in-block offset */

/* ---- block/slot address arithmetic -------------------------------- */

static uint32_t contactAddr(uint16_t index)
{
    uint32_t block = index / VND_CONTACT_PERBLK;
    uint32_t slot  = index % VND_CONTACT_PERBLK;
    return VND_CONTACT_BASE + block * VND_BLOCK_SIZE + slot * sizeof(hd2_vendor_contact_t);
}

static uint32_t channelAddr(uint16_t index)
{
    uint32_t block = index / VND_CHAN_PERBLK;
    uint32_t slot  = index % VND_CHAN_PERBLK;
    return VND_CHAN_BASE + block * VND_BLOCK_SIZE +
           VND_CHAN_SLOT0 + slot * sizeof(hd2_vendor_channel_t);
}

/* ---- public API ---------------------------------------------------- */

int hd2_vendor_contact_present(const hd2_vendor_contact_t *c)
{
    /* Erased flash reads 0xFF; a real contact has a sane type and a name
     * that isn't all-0xFF/all-0x00. */
    if(c->type != HD2_CT_GROUP && c->type != HD2_CT_PRIVATE && c->type != HD2_CT_ALL)
        return 0;
    if((uint8_t)c->name[0] == 0xFFu || c->name[0] == 0x00) return 0;
    return 1;
}

int hd2_vendor_channel_present(const hd2_vendor_channel_t *c)
{
    /* Key off the RX frequency, NOT the name -- valid channels can be unnamed
     * (e.g. cp_ai5qz channel 0 = 145.325 MHz with a blank name).  An erased
     * slot has rx_freq all-0xFF; an unused/zeroed slot all-0x00. */
    int all_ff = 1, all_00 = 1;
    for(int i = 0; i < 4; ++i)
    {
        if(c->rx_freq[i] != 0xFFu) all_ff = 0;
        if(c->rx_freq[i] != 0x00u) all_00 = 0;
    }
    return !(all_ff || all_00);
}

int hd2_vendor_contact_read(uint16_t index, hd2_vendor_contact_t *out)
{
    if(!w25q_hd2_probe()) return -1;
    w25q_hd2_read(contactAddr(index), out, sizeof(*out));
    return 0;
}

int hd2_vendor_channel_read(uint16_t index, hd2_vendor_channel_t *out)
{
    if(!w25q_hd2_probe()) return -1;
    w25q_hd2_read(channelAddr(index), out, sizeof(*out));
    return 0;
}
