/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Codeplug / NVM backend for the Ailunce HD2 (Dahua HR_C7000, C-SKY V2
 * CK803S).  Reads the radio's REAL (Ailunce/Dahua VENDOR-format) codeplug
 * -- channels, priority contacts and zones -- from the external Winbond
 * W25Q SPI-NOR flash (`eflash`, declared in hwconfig.c, driven by the
 * GPIOA bitbang SPI in drivers/SPI/spi_hd2.c through the portable W25Qx.c
 * chip driver) and presents it through the OpenRTX cps_io.h interface, so
 * the UI shows the channels the user programmed with the vendor CPS rather
 * than OpenRTX defaults.
 *
 * ====================================================================
 *  W25Q READS WORK ON HARDWARE
 * --------------------------------------------------------------------
 *  The old "issue #73 / firmware-side read returns flat-0x00" gate that
 *  used to head this file is STALE: settings/VFO persistence
 *  (nvmem_settings_HD2.c) is HW-verified reading and writing this same
 *  W25Q, so byte-addressed reads via nvm_devRead(&eflash, ...) are good.
 *
 *  This file remains inert-safe: every parse validates the vendor record
 *  (slot marker, name sanity, count fields) and any failed / empty /
 *  garbage read returns -1, so the OpenRTX core (state.c) falls back to
 *  its compiled-in defaults instead of crashing or showing junk.
 * ====================================================================
 *
 * Codeplug format choice: VENDOR (Ailunce/Dahua) on-flash layout, decoded
 * in vendor/hd2_infodump/docs/{channels,records,address-space}.md.  We do
 * NOT use the OpenRTX-native cps_header_t + tables layout that
 * platform/drivers/CPS/cps_io_libc.c reads; this reader parses the real
 * vendor records directly so the radio's own programming is honoured.
 *
 * Address mapping (the key insight from the RE work): the vendor CPS
 * protocol's "radio addresses" for the family-0x31 regions are BLOCK
 * INDICES, not byte offsets.  Physical W25Q byte offset = block_index
 * * 0x400.  All region bases below are derived that way and are the
 * absolute flash offsets passed to nvm_devRead().
 *
 * Reused from platform/drivers/NVM/nvmem_CS7000.c (closest sibling --
 * W25Q + colour screen):
 *   - the nvmDevice / nvmPartition / nvmDescriptor / nvmTable wiring,
 *   - nvm_init/terminate ordering.
 *
 * NOTE (2026-06-10): the settings + VFO persistence (nvm_readSettings /
 * nvm_readVfoChannelData / nvm_writeSettings / nvm_writeSettingsAndVfo)
 * moved to nvmem_settings_HD2.c, which stores them in a single 4 kB
 * sector at 0x00FFF000 -- chosen against the now-known VENDOR flash map
 * (see that file's header).  The old fixed slots this file used
 * (0x10000 / 0x11000) were inside an UNPROVEN gap of the vendor layout
 * and are gone.  nvmem_settings_HD2.c implements that half of
 * interfaces/nvmem.h.
 */

#include "interfaces/nvmem.h"
#include "interfaces/cps_io.h"
#include "core/nvmem_device.h"
#include "core/utils.h"
#include "drivers/NVM/W25Qx.h"
#include "drivers/SPI/spi_hd2.h"
#include <string.h>
#include <errno.h>

/* eflash: const struct nvmDevice instantiated in hwconfig.c (W25Q on
 * GPIOA bitbang, CS = PTA18).  We reuse it directly. */
extern const struct nvmDevice eflash;

/* ------------------------------------------------------------------ *
 *  Flash memory map (absolute byte offsets inside eflash)
 *
 *  The HD2's W25Q is large (vendor wires a W25Q512 in 4-byte addr
 *  mode).  The vendor codeplug occupies a number of family-0x31 block
 *  regions; the byte offset of each is `block_index * 0x400`.  The three
 *  regions we parse for the UI are:
 *
 *    Priority contacts  CPS block 0x1B84 -> 0x1B84*0x400 = 0x006E1000
 *    Channel slots      CPS block 0x1BCC -> 0x1BCC*0x400 = 0x006F3000
 *    Zones              CPS block 0x2200 -> 0x2200*0x400 = 0x00880000
 *
 *  See HD2_VND_* below for the per-record packing.  The settings/VFO
 *  sector lives separately at 0xFFF000 (nvmem_settings_HD2.c).
 * ------------------------------------------------------------------ */

#define HD2_SECT_SIZE          4096u

/* ---- Vendor codeplug region bases (absolute W25Q byte offsets) ---- */

/* Priority contacts: 72-byte records, densely packed 14 per 1024-byte
 * block (records.md "Priority Contacts @ radio block 0x1B84"). */
#define HD2_VND_CONTACT_BASE   0x006E1000u
#define HD2_VND_CONTACT_STRIDE 72u
#define HD2_VND_CONTACT_PERBLK 14u

/* Channel slots: 176-byte (0xB0) records, 5 per 1024-byte block, FIRST
 * slot at block offset 0x80 (channels.md "Channels @ radio blocks
 * 0x1BCC").  Within each 0x400 block the 5 slots sit at +0x80, +0x130,
 * +0x1E0, +0x290, +0x340 (0x80 + n*0xB0). */
#define HD2_VND_CHAN_BASE      0x006F3000u
#define HD2_VND_CHAN_STRIDE    0xB0u
#define HD2_VND_CHAN_PERBLK    5u
#define HD2_VND_CHAN_HDRGAP    0x80u

/* Zones: 145-byte (0x91) records, densely packed (records.md "Zones @
 * radio blocks 0x2200+"). */
#define HD2_VND_ZONE_BASE      0x00880000u
#define HD2_VND_ZONE_STRIDE    0x91u

/* Safety caps so a corrupt count never walks off into hyperspace.  Far
 * above the documented UI maxima (3000 ch / 256 zones / 64 ch-per-zone)
 * would still be a finite, bounded scan; these are the practical caps
 * the OpenRTX UI iterates against. */
#define HD2_VND_CHAN_MAX       3000u
#define HD2_VND_ZONE_MAX       256u
#define HD2_VND_ZONE_CHMAX     64u
#define HD2_VND_CONTACT_MAX    1024u

/* Block index of the channels region, used only by the legacy
 * partition table below (kept so the nvmTable wiring is unchanged). */
#define HD2_CALIB_BASE         0x00000000u
#define HD2_CALIB_SIZE         0x00010000u   /* 64 kB (calib, TBD)     */

static const struct nvmPartition memPartitions[] =
{
    {
        .offset = HD2_CALIB_BASE,
        .size   = HD2_CALIB_SIZE,
    },
};

static const struct nvmDescriptor extMem[] =
{
    {
        .name       = "External flash",
        .dev        = &eflash,
        .baseAddr   = 0x00000000,
        .size       = 0x04000000,   /* 64 MB, W25Q512 (256 Mbit x2 die) */
        .nbPart     = sizeof(memPartitions) / sizeof(struct nvmPartition),
        .partitions = memPartitions,
    },
};

const struct nvmTable nvmTab =
{
    .areas   = extMem,
    .nbAreas = sizeof(extMem) / sizeof(struct nvmDescriptor),
};

/* ================================================================== *
 *  NVM driver lifecycle                                              *
 * ================================================================== */

void nvm_init()
{
    /* The GPIOA bitbang SPI bus is brought up by spi_hd2_init() in the
     * HD2 main.c bring-up path BEFORE this runs; calling it again here is
     * idempotent (it just re-parks the pins).  W25Qx_init() wakes the
     * chip out of deep-power-down so reads/writes work.
     *
     * NOTE (#73): W25Qx_init()'s wakeup is currently commented out in
     * main.c because the firmware-side read returns 0x00; once #73 is
     * fixed this is the single place that owns chip bring-up for the
     * codeplug backend. */
    spi_hd2_init();
    W25Qx_init(&eflash);
}

void nvm_terminate()
{
    W25Qx_terminate(&eflash);
}

void nvm_readCalibData(void *buf)
{
    /* Calibration layout in the HD2 flash is not yet reverse engineered
     * (separate task).  Leave the caller's buffer untouched so the core
     * uses its compiled-in defaults. */
    (void) buf;
}

void nvm_readHwInfo(hwInfo_t *info)
{
    (void) info;
}


/* ================================================================== *
 *  Settings + VFO: in nvmem_settings_HD2.c (single 4 kB sector at    *
 *  0x00FFF000, versioned header + CRC, JEDEC-gated, IRQ-locked SPI   *
 *  transactions).  Compiled into both HD2 builds.                    *
 * ================================================================== */


/* ================================================================== *
 *  VENDOR (Ailunce/Dahua) codeplug reader (cps_io.h backend)         *
 *                                                                    *
 *  Parses the radio's real on-flash records -- channel slots,        *
 *  priority contacts and zones -- and maps them into the OpenRTX     *
 *  channel_t / contact_t / bankHdr_t structures the UI iterates.     *
 *  Record formats: vendor/hd2_infodump/docs/{channels,records}.md.   *
 *                                                                    *
 *  Presence policy: a record is "present" iff its own marker/count   *
 *  validates AND its name is not all-0xFF/all-0x00.  The authoritative*
 *  presence source is the dual channel-presence bitmap in vfo_config *
 *  (physical 0x800000, see address-space.md), but the per-record     *
 *  marker check is simpler and sufficient for a read-only UI -- an    *
 *  empty/garbage slot returns -1 and the core skips it.              *
 * ================================================================== */

/* --- helpers ----------------------------------------------------- */

/* Convert a little-endian packed-BCD field to its decimal value.
 * The vendor stores frequencies as 4-byte LE packed BCD (8 digits);
 * the decoded decimal value is in units of 10 Hz, so callers multiply
 * by 10 to obtain Hz.  Returns 0xFFFFFFFF if any nibble is not a valid
 * BCD digit (used to detect blank 0xFF.. fields). */
static uint32_t bcd_to_u32(const uint8_t *bytes, size_t nbytes)
{
    uint32_t val = 0;
    for(size_t i = nbytes; i > 0; i--)        /* MSByte first  */
    {
        uint8_t b  = bytes[i - 1];
        uint8_t hi = (b >> 4) & 0x0F;
        uint8_t lo =  b       & 0x0F;
        if(hi > 9 || lo > 9)
            return 0xFFFFFFFFu;               /* not valid BCD */
        val = (val * 100u) + (hi * 10u) + lo;
    }
    return val;
}

/* True if `len` bytes at `p` are all 0xFF or all 0x00 (blank slot). */
static bool isBlank(const uint8_t *p, size_t len)
{
    bool allFF = true, all00 = true;
    for(size_t i = 0; i < len; i++)
    {
        if(p[i] != 0xFF) allFF = false;
        if(p[i] != 0x00) all00 = false;
    }
    return allFF || all00;
}

/* Copy a fixed-width vendor name (ASCII, 0x00/0xFF padded) into a NUL-
 * terminated OpenRTX name[] buffer, stopping at the first pad byte. */
static void copyName(char *dst, size_t dstSize, const uint8_t *src, size_t srcLen)
{
    size_t n = 0;
    size_t lim = (srcLen < dstSize - 1) ? srcLen : dstSize - 1;
    for(; n < lim; n++)
    {
        uint8_t c = src[n];
        if(c == 0x00 || c == 0xFF)
            break;
        dst[n] = (char) c;
    }
    dst[n] = '\0';
}

/* Decode a 2-byte vendor tone field into an OpenRTX rxTone/txTone index
 * (into ctcss_tone[]) + enable flag.  Only CTCSS is representable by the
 * OpenRTX fmInfo_t (it has no DCS index space); DCS and unknown values
 * are reported as "disabled".  channels.md tone encoding:
 *   hi < 0x80  -> CTCSS, freq = BCD(hi,lo) * 0.1 Hz
 *   hi & 0x80  -> DCS (normal/inverted) -- not represented here
 *   0xFFFF     -> none */
static void decodeTone(const uint8_t *p, uint8_t *toneIdx, uint8_t *toneEn)
{
    *toneIdx = 0;
    *toneEn  = 0;

    uint8_t lo = p[0];
    uint8_t hi = p[1];

    if(hi == 0xFF && lo == 0xFF)              /* none */
        return;
    if(hi & 0x80)                             /* DCS -- unsupported  */
        return;

    /* CTCSS: BCD(hi:lo) is the frequency in tenths of Hz, which is the
     * exact unit ctcss_tone[] stores (e.g. 1713 == 171.3 Hz). */
    uint8_t buf[2] = { lo, hi };
    uint32_t tenths = bcd_to_u32(buf, 2);
    if(tenths == 0xFFFFFFFFu)
        return;

    uint8_t idx = ctcssFreqToIndex((uint16_t) tenths);
    if(idx == 255)                            /* not in OpenRTX table */
        return;

    *toneIdx = idx;
    *toneEn  = 1;
}

/* Absolute W25Q byte offset of channel slot `pos` (global slot index).
 * Packing (channels.md): 5 slots per 1024-byte block, first slot at
 * block offset 0x80, slot stride 0xB0:
 *   block      = pos / 5
 *   inBlock    = pos % 5
 *   byteOffset = base + block*0x400 + 0x80 + inBlock*0xB0 */
static uint32_t channelSlotOffset(uint16_t pos)
{
    uint32_t block   = pos / HD2_VND_CHAN_PERBLK;
    uint32_t inBlock = pos % HD2_VND_CHAN_PERBLK;
    return HD2_VND_CHAN_BASE + block * 0x400u
         + HD2_VND_CHAN_HDRGAP + inBlock * HD2_VND_CHAN_STRIDE;
}

/* Absolute W25Q byte offset of priority contact `pos`. 14 per block,
 * densely packed from block offset 0 (records.md). */
static uint32_t contactSlotOffset(uint16_t pos)
{
    uint32_t block   = pos / HD2_VND_CONTACT_PERBLK;
    uint32_t inBlock = pos % HD2_VND_CONTACT_PERBLK;
    return HD2_VND_CONTACT_BASE + block * 0x400u
         + inBlock * HD2_VND_CONTACT_STRIDE;
}

/* Absolute W25Q byte offset of zone `pos`.  Zones are densely packed
 * (NOT one-per-block) at stride 0x91 from the region base (records.md). */
static uint32_t zoneSlotOffset(uint16_t pos)
{
    return HD2_VND_ZONE_BASE + (uint32_t) pos * HD2_VND_ZONE_STRIDE;
}

/* --- cps_io.h lifecycle ------------------------------------------ */

int cps_open(char *cps_name)
{
#ifdef HD2_CODEPLUG_STUB
    return -1;   /* codeplug reader taken offline (isolation build) */
#endif

    /* The vendor format has no global header/magic to validate, and the
     * per-record readers each re-validate, so a light sanity read of the
     * first channel slot is enough to confirm the bus is alive.  Failure
     * here is non-fatal: the read entry points still guard themselves. */
    (void) cps_name;

    uint8_t marker[4];
    if(nvm_devRead(&eflash, channelSlotOffset(0), marker, sizeof(marker)) < 0)
        return -1;

    return 0;
}

void cps_close()
{
}

int cps_create(char *cps_name)
{
    /* We do NOT author the vendor codeplug format -- it is created by the
     * Ailunce CPS over USB.  Report "not supported" so the core never
     * tries to lay one down (and never erases vendor data). */
    (void) cps_name;
    errno = ENOTSUP;
    return -1;
}

/* --- channels ---------------------------------------------------- */

int cps_readChannel(channel_t *channel, uint16_t pos)
{
#ifdef HD2_CODEPLUG_STUB
    return -1;   /* codeplug reader taken offline (isolation build) */
#endif

    if(pos >= HD2_VND_CHAN_MAX)
        return -1;

    uint8_t raw[HD2_VND_CHAN_STRIDE];
    if(nvm_devRead(&eflash, channelSlotOffset(pos), raw, sizeof(raw)) < 0)
        return -1;

    /* +0x00 4B marker: 0xFFFFFFFF == populated slot. */
    if(raw[0x00] != 0xFF || raw[0x01] != 0xFF ||
       raw[0x02] != 0xFF || raw[0x03] != 0xFF)
        return -1;

    /* +0x04 10B name: an all-0xFF/all-0x00 name => empty/garbage slot. */
    if(isBlank(&raw[0x04], 10))
        return -1;

    memset(channel, 0x00, sizeof(*channel));

    copyName(channel->name, sizeof(channel->name), &raw[0x04], 10);

    /* Frequencies: 4-byte LE packed BCD, decoded value * 10 = Hz. */
    uint32_t rxBcd = bcd_to_u32(&raw[0x14], 4);
    uint32_t txBcd = bcd_to_u32(&raw[0x18], 4);
    if(rxBcd == 0xFFFFFFFFu)                  /* invalid RX freq slot */
        return -1;
    channel->rx_frequency = rxBcd * 10u;
    channel->tx_frequency = (txBcd == 0xFFFFFFFFu) ? (rxBcd * 10u)
                                                   : (txBcd * 10u);

    /* Mode: +0x21 bit6 (0x40) set => DMR, clear => FM (THE mode bit). */
    bool isDmr = (raw[0x21] & 0x40) != 0;
    channel->mode = isDmr ? OPMODE_DMR : OPMODE_FM;

    /* Bandwidth: +0x29 bit6 (0x40) set => wide, clear => narrow. */
    channel->bandwidth = (raw[0x29] & 0x40) ? BW_25 : BW_12_5;

    channel->rx_only = false;

    /* Power: +0x21 bits3:2 (0x0C) plus +0x29 bit3 (0x08, 0.5W xtra-low).
     *   Low    = 0x00 -> 1000 mW
     *   Medium = 0x04 -> 2500 mW
     *   High   = 0x08 -> 5000 mW
     *   XtraLow= 0x04 + (+0x29 bit3) ->  500 mW
     * Values chosen as sane representatives; the radio's actual PA levels
     * are calibrated elsewhere. */
    if(raw[0x29] & 0x08)
        channel->power = 500u;                /* 0.5 W extra-low */
    else
    {
        switch(raw[0x21] & 0x0C)
        {
            case 0x00: channel->power = 1000u; break;   /* Low    */
            case 0x04: channel->power = 2500u; break;   /* Medium */
            case 0x08: channel->power = 5000u; break;   /* High   */
            default:   channel->power = 1000u; break;
        }
    }

    /* No scan-list / group-list cross-tables are wired in this reader. */
    channel->scanList_index  = 0;
    channel->groupList_index = 0;

    if(isDmr)
    {
        /* +0x2A packed: high nibble = colour code (0..15);
         *               low nibble bit0 = timeslot (0=TS1, 1=TS2). */
        uint8_t cc = (raw[0x2A] >> 4) & 0x0F;
        channel->dmr.rxColorCode = cc;
        channel->dmr.txColorCode = cc;
        channel->dmr.dmr_timeslot = (raw[0x2A] & 0x01) ? 2 : 1;

        /* +0x1C 4B LE = contact DMR ID.  OpenRTX wants a contact *index*;
         * we have no DMR-ID->index map here, so leave it unresolved (0).
         * The contact table (cps_readContact) is exposed independently. */
        channel->dmr.contact_index = 0;
    }
    else
    {
        /* FM tones: +0x24 rx, +0x26 tx (CTCSS only; DCS unsupported). */
        decodeTone(&raw[0x24], &channel->fm.rxTone, &channel->fm.rxToneEn);
        decodeTone(&raw[0x26], &channel->fm.txTone, &channel->fm.txToneEn);
    }

    return 0;
}

/* --- priority contacts ------------------------------------------- */

int cps_readContact(contact_t *contact, uint16_t pos)
{
#ifdef HD2_CODEPLUG_STUB
    return -1;   /* codeplug reader taken offline (isolation build) */
#endif

    if(pos >= HD2_VND_CONTACT_MAX)
        return -1;

    uint8_t raw[HD2_VND_CONTACT_STRIDE];
    if(nvm_devRead(&eflash, contactSlotOffset(pos), raw, sizeof(raw)) < 0)
        return -1;

    /* Empty priority-contact slots are 0xFF-filled (records.md). A blank
     * name area means no record here. */
    if(isBlank(&raw[0x05], 16))
        return -1;

    memset(contact, 0x00, sizeof(*contact));
    contact->mode = OPMODE_DMR;               /* HD2 contacts are DMR */

    copyName(contact->name, sizeof(contact->name), &raw[0x05], 16);

    /* +0x00 4B LE DMR ID. */
    contact->info.dmr.id = (uint32_t) raw[0x00]
                         | ((uint32_t) raw[0x01] << 8)
                         | ((uint32_t) raw[0x02] << 16)
                         | ((uint32_t) raw[0x03] << 24);

    /* +0x04 type: 0x04 group, 0x05 private, 0x06 all. */
    switch(raw[0x04])
    {
        case 0x04: contact->info.dmr.contactType = GROUP;   break;
        case 0x05: contact->info.dmr.contactType = PRIVATE; break;
        case 0x06: contact->info.dmr.contactType = ALL;     break;
        default:   contact->info.dmr.contactType = GROUP;   break;
    }
    contact->info.dmr.rx_tone = 0;

    return 0;
}

/* --- zones (OpenRTX "banks") ------------------------------------- */

int cps_readBankHeader(bankHdr_t *b_header, uint16_t pos)
{
#ifdef HD2_CODEPLUG_STUB
    return -1;   /* codeplug reader taken offline (isolation build) */
#endif

    if(pos >= HD2_VND_ZONE_MAX)
        return -1;

    uint8_t raw[HD2_VND_ZONE_STRIDE];
    if(nvm_devRead(&eflash, zoneSlotOffset(pos), raw, sizeof(raw)) < 0)
        return -1;

    /* +0x00 1B count: 0xFF == empty slot. */
    uint8_t count = raw[0x00];
    if(count == 0xFF || count == 0x00 || count > HD2_VND_ZONE_CHMAX)
        return -1;

    /* +0x81 16B name: empty (all-0xFF/0x00) => skip. */
    if(isBlank(&raw[0x81], 16))
        return -1;

    memset(b_header, 0x00, sizeof(*b_header));
    copyName(b_header->name, sizeof(b_header->name), &raw[0x81], 16);
    b_header->ch_count = count;

    return 0;
}

int cps_readBankData(uint16_t bank_pos, uint16_t pos)
{
#ifdef HD2_CODEPLUG_STUB
    return -1;   /* codeplug reader taken offline (isolation build) */
#endif

    if(bank_pos >= HD2_VND_ZONE_MAX || pos >= HD2_VND_ZONE_CHMAX)
        return -1;

    uint8_t raw[HD2_VND_ZONE_STRIDE];
    if(nvm_devRead(&eflash, zoneSlotOffset(bank_pos), raw, sizeof(raw)) < 0)
        return -1;

    uint8_t count = raw[0x00];
    if(count == 0xFF || pos >= count || count > HD2_VND_ZONE_CHMAX)
        return -1;

    /* +0x01 channel list: count * 2-byte LE channel indices (0-based). */
    uint32_t i = 0x01u + (uint32_t) pos * 2u;
    uint32_t ch_index = (uint32_t) raw[i] | ((uint32_t) raw[i + 1] << 8);

    return (int) ch_index;
}


/* ================================================================== *
 *  Codeplug mutators -- NOT YET IMPLEMENTED                          *
 *                                                                    *
 *  In-place edit of a NOR-flash codeplug needs read-modify-erase-    *
 *  rewrite of whole 4 kB sectors plus the variable-length push-down  *
 *  bookkeeping that cps_io_libc.c does with ftell()/_pushDown().     *
 *  None of these are referenced by the current HD2 link (UI edit     *
 *  paths are not wired), and they cannot be exercised before #73, so *
 *  they return -1 for now.  Implement once read is live-verified.    *
 * ================================================================== */

int cps_writeContact(contact_t contact, uint16_t pos)
{ (void)contact; (void)pos; return -1; }

int cps_writeChannel(channel_t channel, uint16_t pos)
{ (void)channel; (void)pos; return -1; }

int cps_writeBankHeader(bankHdr_t b_header, uint16_t pos)
{ (void)b_header; (void)pos; return -1; }

int cps_writeBankData(uint32_t ch, uint16_t bank_pos, uint16_t pos)
{ (void)ch; (void)bank_pos; (void)pos; return -1; }

int cps_insertContact(contact_t contact, uint16_t pos)
{ (void)contact; (void)pos; return -1; }

int cps_insertChannel(channel_t channel, uint16_t pos)
{ (void)channel; (void)pos; return -1; }

int cps_insertBankHeader(bankHdr_t b_header, uint16_t pos)
{ (void)b_header; (void)pos; return -1; }

int cps_insertBankData(uint32_t ch, uint16_t bank_pos, uint16_t pos)
{ (void)ch; (void)bank_pos; (void)pos; return -1; }

int cps_deleteContact(uint16_t pos)
{ (void)pos; return -1; }

int cps_deleteChannel(channel_t channel, uint16_t pos)
{ (void)channel; (void)pos; return -1; }

int cps_deleteBankHeader(uint16_t pos)
{ (void)pos; return -1; }

int cps_deleteBankData(uint16_t bank_pos, uint16_t pos)
{ (void)bank_pos; (void)pos; return -1; }
