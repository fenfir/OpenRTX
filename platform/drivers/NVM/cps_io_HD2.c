/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenRTX-native codeplug (cps_io.h) backend for the Ailunce HD2, stored on
 * the external W25Q512 SPI-NOR over the HW SPI0 bus (drivers/SPI/spi_hd2.c).
 *
 * This is the OpenRTX codeplug format ("RTXC" magic, see core/cps.h) -- NOT
 * the factory Ailunce/Dahua format.  It gives full read AND write/edit
 * (channels, contacts, banks/zones) for on-radio CPS use; a fresh radio gets
 * an empty codeplug laid down by cps_create() on first open.
 *
 * STORAGE MODEL
 *   The on-flash image is the exact byte layout cps_io_libc.c uses (header,
 *   then contacts[], channels[], a per-bank uint32 offset table, then the
 *   bank-header + bank-data blocks).  To reuse that proven, fiddly push-down
 *   logic verbatim, the codeplug is worked on as an in-RAM "memfile":
 *     - cps_open()  mallocs a buffer and reads the region into it; reads are
 *       then served from RAM.
 *     - any mutating op edits the RAM image, then commits the whole region
 *       back to flash (sector-erase + page-program).  Edits are human-paced,
 *       so a full-region rewrite per edit is simple and safe.
 *   Buffer grows on insert up to CPS_REGION_SIZE.  Reads cost no flash I/O
 *   while open; the resident footprint is the actual codeplug size.
 *
 * FLASH REGION: 64 kB at 0x00FEF000, the 16 sectors immediately below the
 * settings sector (0x00FFF000, nvmem_settings_HD2.c) -- the same upper
 * neighborhood validated free of live vendor data.
 *
 * The W25Q access primitives below mirror nvmem_settings_HD2.c (4-byte-addr
 * opcodes, JEDEC gate, IRQ-locked transactions).  They are intentionally a
 * local copy: the settings driver is validated and self-contained, and a
 * shared low-level W25Q driver is a future refactor (TODO) rather than a
 * change to proven code.
 */

#include "interfaces/cps_io.h"
#include "drivers/SPI/spi_hd2.h"
#include "hd2_regs.h"                /* GPIOA_DR + hd2_irq_save/restore (inline) */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* ================================================================== *
 *  W25Q512 low-level access (HW SPI0; see header note)               *
 * ================================================================== */

#define FLASH_CS_MASK   (1u << 18)            /* CS# = GPIOA.18, active low */
#define MMIO_SYNC()     __asm__ volatile ("sync" ::: "memory")

#define HD2_PAGE_SIZE   256u
#define HD2_SECTOR_SIZE 4096u

#define OPC_WREN        0x06u
#define OPC_RDSR1       0x05u
#define OPC_JEDEC_ID    0x9Fu
#define OPC_WAKEUP      0xABu
#define OPC_READ_4B     0x13u
#define OPC_PROG_4B     0x12u
#define OPC_ERASE_4K_4B 0x21u

/* Codeplug region on the W25Q. */
#define CPS_BASE         0x00FEF000u
#define CPS_REGION_SIZE  0x00010000u          /* 64 kB = 16 x 4 kB sectors */

static void flashXfer(const uint8_t *cmd, size_t cmdLen,
                      const void *txData, size_t txLen,
                      void *rxData, size_t rxLen)
{
    uint32_t irq = hd2_irq_save();
    GPIOA_DR &= ~FLASH_CS_MASK;
    MMIO_SYNC();
    nvm_spi.transfer(&nvm_spi, cmd, NULL, cmdLen);
    if(txLen > 0) nvm_spi.transfer(&nvm_spi, txData, NULL, txLen);
    if(rxLen > 0) nvm_spi.transfer(&nvm_spi, NULL, rxData, rxLen);
    GPIOA_DR |= FLASH_CS_MASK;
    MMIO_SYNC();
    hd2_irq_restore(irq);
}

static int flashWaitReady(uint32_t timeoutMs)
{
    uint32_t ticks = timeoutMs * 2;           /* ~500 us per poll */
    while(ticks > 0)
    {
        uint8_t cmd = OPC_RDSR1, sr = 0xFF;
        flashXfer(&cmd, 1, NULL, 0, &sr, 1);
        if((sr & 0x01u) == 0u) return 0;      /* WIP clear */
        for(volatile unsigned i = 0; i < 4000u; ++i) { }
        ticks--;
    }
    return -1;
}

static bool flashProbe(void)
{
    spi_hd2_init();
    uint8_t cmd = OPC_WAKEUP;
    flashXfer(&cmd, 1, NULL, 0, NULL, 0);
    for(volatile unsigned i = 0; i < 8000u; ++i) { }   /* tRES2 settle */
    uint8_t id[3] = { 0, 0, 0 };
    cmd = OPC_JEDEC_ID;
    flashXfer(&cmd, 1, NULL, 0, id, 3);
    return (id[0] == 0xEF) && (id[1] == 0x40) && (id[2] == 0x20);
}

static void flashWriteEnable(void)
{
    uint8_t cmd = OPC_WREN;
    flashXfer(&cmd, 1, NULL, 0, NULL, 0);
}

static void flashRead(uint32_t addr, void *buf, size_t len)
{
    const uint8_t cmd[] = { OPC_READ_4B,
        (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),  (uint8_t)(addr) };
    flashXfer(cmd, sizeof(cmd), NULL, 0, buf, len);
}

static int flashEraseSector(uint32_t addr)
{
    const uint8_t cmd[] = { OPC_ERASE_4K_4B,
        (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),  (uint8_t)(addr) };
    flashWriteEnable();
    flashXfer(cmd, sizeof(cmd), NULL, 0, NULL, 0);
    return flashWaitReady(500);               /* sector erase: typ 45 ms */
}

static int flashProgram(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *data = (const uint8_t *) buf;
    while(len > 0)
    {
        size_t pageRoom = HD2_PAGE_SIZE - (addr & (HD2_PAGE_SIZE - 1));
        size_t chunk    = (len < pageRoom) ? len : pageRoom;
        const uint8_t cmd[] = { OPC_PROG_4B,
            (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
            (uint8_t)(addr >> 8),  (uint8_t)(addr) };
        flashWriteEnable();
        flashXfer(cmd, sizeof(cmd), data, chunk, NULL, 0);
        if(flashWaitReady(10) < 0) return -1;
        addr += chunk; data += chunk; len -= chunk;
    }
    return 0;
}

/* ================================================================== *
 *  In-RAM "memfile" -- the open codeplug image                       *
 * ================================================================== */

static uint8_t *mf_buf  = NULL;   /* malloc'd region image (NULL = closed) */
static uint32_t mf_pos  = 0;      /* cursor                                */
static uint32_t mf_size = 0;      /* meaningful data length                */
static bool     mf_dirty = false;

static inline void mf_seek_set(uint32_t off) { mf_pos = off; }
static inline void mf_seek_cur(uint32_t off) { mf_pos += off; }

static void mf_read(void *dst, uint32_t len)
{
    if(mf_pos + len > CPS_REGION_SIZE) len = (mf_pos < CPS_REGION_SIZE) ? CPS_REGION_SIZE - mf_pos : 0;
    memcpy(dst, mf_buf + mf_pos, len);
    mf_pos += len;
}

static void mf_write(const void *src, uint32_t len)
{
    if(mf_pos + len > CPS_REGION_SIZE) return;          /* refuse overflow */
    memcpy(mf_buf + mf_pos, src, len);
    mf_pos += len;
    if(mf_pos > mf_size) mf_size = mf_pos;
    mf_dirty = true;
}

/* Move [offset .. mf_size) up by `amount` bytes to open a hole (insert). */
static int mf_pushdown(uint32_t offset, uint32_t amount)
{
    if(mf_size + amount > CPS_REGION_SIZE) return -1;   /* would overflow region */
    if(offset < mf_size)
        memmove(mf_buf + offset + amount, mf_buf + offset, mf_size - offset);
    mf_size += amount;
    mf_dirty = true;
    return 0;
}

/* Commit the whole region image to flash (erase the sectors it spans, then
 * reprogram).  Called after a mutating op completes. */
static int mf_commit(void)
{
    uint32_t span = (mf_size + HD2_SECTOR_SIZE - 1u) & ~(HD2_SECTOR_SIZE - 1u);
    if(span == 0u) span = HD2_SECTOR_SIZE;
    if(span > CPS_REGION_SIZE) span = CPS_REGION_SIZE;
    if(!flashProbe()) return -1;
    for(uint32_t off = 0; off < span; off += HD2_SECTOR_SIZE)
        if(flashEraseSector(CPS_BASE + off) < 0) return -1;
    if(flashProgram(CPS_BASE, mf_buf, mf_size) < 0) return -1;
    mf_dirty = false;
    return 0;
}

/* ================================================================== *
 *  Header + bank-offset helpers (ported from cps_io_libc.c)          *
 * ================================================================== */

static int _readHeader(cps_header_t *header)
{
    mf_seek_set(0);
    mf_read(header, sizeof(cps_header_t));
    if(header->magic != CPS_MAGIC) return -1;
    if(((header->version_number & 0xff00) >> 8) != CPS_VERSION_MAJOR ||
        (header->version_number & 0x00ff) > CPS_VERSION_MINOR)
        return -1;
    return 0;
}

static int _writeHeader(cps_header_t header)
{
    mf_seek_set(0);
    mf_write(&header, sizeof(cps_header_t));
    return 0;
}

/* End-of-data offset for bank `pos` (== start of bank pos's header).  For
 * pos==b_count this is the end of all bank data (the image size).  Mirrors
 * cps_io_libc _getBankDataOffset with the memfile cursor. */
static long _getBankDataOffset(uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.b_count + 1) return -1;

    uint32_t tableBase = sizeof(cps_header_t) +
                         header.ct_count * sizeof(contact_t) +
                         header.ch_count * sizeof(channel_t);

    if(pos == header.b_count)
    {
        if(header.b_count == 0)
            return tableBase + sizeof(uint32_t);
        mf_seek_set(tableBase + (header.b_count - 1) * sizeof(uint32_t));
        uint32_t offset = 0;
        mf_read(&offset, sizeof(uint32_t));
        long bdata_pos = (long)mf_pos;
        bankHdr_t last_bank = { 0 };
        cps_readBankHeader(&last_bank, header.b_count - 1);
        return bdata_pos + offset + sizeof(bankHdr_t) +
               last_bank.ch_count * sizeof(uint32_t);
    }
    mf_seek_set(tableBase + pos * sizeof(uint32_t));
    uint32_t offset = 0;
    mf_read(&offset, sizeof(uint32_t));
    return (long)mf_pos + (header.b_count - pos - 1) * sizeof(uint32_t) + offset;
}

/* ================================================================== *
 *  cps_io.h interface                                                *
 * ================================================================== */

int cps_open(char *cps_name)
{
    (void) cps_name;
    if(mf_buf == NULL)
    {
        mf_buf = (uint8_t *) malloc(CPS_REGION_SIZE);
        if(mf_buf == NULL) return -1;
    }
    if(!flashProbe()) { free(mf_buf); mf_buf = NULL; return -1; }

    /* Load the header first to learn the meaningful size, then the body. */
    flashRead(CPS_BASE, mf_buf, sizeof(cps_header_t));
    cps_header_t hdr;
    memcpy(&hdr, mf_buf, sizeof(hdr));
    if(hdr.magic != CPS_MAGIC)
    {
        /* Fresh / unformatted region: lay down an empty codeplug. */
        free(mf_buf); mf_buf = NULL;
        if(cps_create(NULL) != 0) return -1;
        mf_buf = (uint8_t *) malloc(CPS_REGION_SIZE);
        if(mf_buf == NULL) return -1;
        flashRead(CPS_BASE, mf_buf, sizeof(cps_header_t));
        memcpy(&hdr, mf_buf, sizeof(hdr));
    }
    /* Read the full region; mf_size is then trimmed to the real end-of-data. */
    flashRead(CPS_BASE, mf_buf, CPS_REGION_SIZE);
    mf_size = CPS_REGION_SIZE;
    mf_dirty = false;
    long end = _getBankDataOffset(hdr.b_count);
    if(end > 0 && (uint32_t)end <= CPS_REGION_SIZE) mf_size = (uint32_t)end;
    return 0;
}

void cps_close()
{
    if(mf_buf != NULL) { free(mf_buf); mf_buf = NULL; }
    mf_size = 0; mf_dirty = false;
}

int cps_create(char *cps_name)
{
    (void) cps_name;
    cps_header_t header = { 0 };
    header.magic = CPS_MAGIC;
    header.version_number = (CPS_VERSION_MAJOR << 8) | CPS_VERSION_MINOR;
    strncpy(header.author, "HD2", CPS_STR_SIZE - 1);
    strncpy(header.descr,  "OpenRTX HD2 codeplug", CPS_STR_SIZE - 1);
    header.timestamp = 0;
    header.ct_count = 0;
    header.ch_count = 0;
    header.b_count  = 0;

    if(!flashProbe()) return -1;
    if(flashEraseSector(CPS_BASE) < 0) return -1;
    return flashProgram(CPS_BASE, &header, sizeof(header));
}

int cps_readContact(contact_t *contact, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ct_count) return -1;
    mf_seek_cur(pos * sizeof(contact_t));
    mf_read(contact, sizeof(contact_t));
    return 0;
}

int cps_readChannel(channel_t *channel, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ch_count) return -1;
    mf_seek_cur(header.ct_count * sizeof(contact_t) + pos * sizeof(channel_t));
    mf_read(channel, sizeof(channel_t));
    return 0;
}

int cps_readBankHeader(bankHdr_t *b_header, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.b_count) return -1;
    mf_seek_cur(header.ct_count * sizeof(contact_t) +
                header.ch_count * sizeof(channel_t) +
                pos * sizeof(uint32_t));
    uint32_t offset = 0;
    mf_read(&offset, sizeof(uint32_t));
    mf_seek_cur((header.b_count - pos - 1) * sizeof(uint32_t) + offset);
    mf_read(b_header, sizeof(bankHdr_t));
    return 0;
}

int cps_readBankData(uint16_t bank_pos, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(bank_pos >= header.b_count) return -1;
    mf_seek_cur(header.ct_count * sizeof(contact_t) +
                header.ch_count * sizeof(channel_t) +
                bank_pos * sizeof(uint32_t));
    uint32_t offset = 0;
    mf_read(&offset, sizeof(uint32_t));
    mf_seek_cur((header.b_count - bank_pos - 1) * sizeof(uint32_t) + offset);
    bankHdr_t b_header = { 0 };
    mf_read(&b_header, sizeof(bankHdr_t));
    if(pos >= b_header.ch_count) return -1;
    mf_seek_cur(pos * sizeof(uint32_t));
    uint32_t ch_index = 0;
    mf_read(&ch_index, sizeof(uint32_t));
    return (int) ch_index;
}

int cps_writeContact(contact_t contact, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ct_count) return -1;
    mf_seek_cur(pos * sizeof(contact_t));
    mf_write(&contact, sizeof(contact_t));
    return mf_commit();
}

int cps_writeChannel(channel_t channel, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ch_count) return -1;
    mf_seek_cur(header.ct_count * sizeof(contact_t) + pos * sizeof(channel_t));
    mf_write(&channel, sizeof(channel_t));
    return mf_commit();
}

int cps_writeBankHeader(bankHdr_t b_header, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.b_count) return -1;
    mf_seek_cur(header.ct_count * sizeof(contact_t) +
                header.ch_count * sizeof(channel_t) +
                pos * sizeof(uint32_t));
    uint32_t offset = 0;
    mf_read(&offset, sizeof(uint32_t));
    mf_seek_cur((header.b_count - pos - 1) * sizeof(uint32_t) + offset);
    mf_write(&b_header, sizeof(bankHdr_t));
    return mf_commit();
}

int cps_writeBankData(uint32_t ch, uint16_t bank_pos, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(bank_pos >= header.b_count) return -1;
    mf_seek_cur(header.ct_count * sizeof(contact_t) +
                header.ch_count * sizeof(channel_t) +
                bank_pos * sizeof(uint32_t));
    uint32_t offset = 0;
    mf_read(&offset, sizeof(uint32_t));
    mf_seek_cur((header.b_count - bank_pos - 1) * sizeof(uint32_t) + offset);
    bankHdr_t b_header = { 0 };
    uint32_t hdr_pos = mf_pos;
    mf_read(&b_header, sizeof(bankHdr_t));
    if(pos >= b_header.ch_count) return -1;
    mf_seek_set(hdr_pos + sizeof(bankHdr_t) + pos * sizeof(uint32_t));
    mf_write(&ch, sizeof(uint32_t));
    return mf_commit();
}

/* ---- insert (ported from cps_io_libc.c; numbering helpers inline) ---- */

static int _updateCtNumbering(uint16_t pos, bool add)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    for(int i = 0; i < header.ch_count; i++)
    {
        channel_t c = { 0 };
        cps_readChannel(&c, i);
        bool changed = false;
        if(c.mode == OPMODE_M17 && c.m17.contact_index >= pos)
        { c.m17.contact_index += add ? 1 : -1; changed = true; }
        if(c.mode == OPMODE_DMR && c.dmr.contact_index >= pos)
        { c.dmr.contact_index += add ? 1 : -1; changed = true; }
        if(changed)
        {   /* in-RAM write; the caller commits once at the end */
            mf_seek_set(sizeof(cps_header_t) + header.ct_count * sizeof(contact_t) +
                        i * sizeof(channel_t));
            mf_write(&c, sizeof(channel_t));
        }
    }
    return 0;
}

int cps_insertContact(contact_t contact, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ct_count + 1) return -1;
    uint32_t ct_pos = sizeof(cps_header_t) + pos * sizeof(contact_t);
    if(mf_pushdown(ct_pos, sizeof(contact_t)) < 0) return -1;
    mf_seek_set(ct_pos);
    mf_write(&contact, sizeof(contact_t));
    header.ct_count++;
    _writeHeader(header);
    _updateCtNumbering(pos, true);
    return mf_commit();
}

int cps_insertChannel(channel_t channel, uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ch_count + 1) return -1;
    uint32_t ch_pos = sizeof(cps_header_t) + header.ct_count * sizeof(contact_t) +
                      pos * sizeof(channel_t);
    if(mf_pushdown(ch_pos, sizeof(channel_t)) < 0) return -1;
    mf_seek_set(ch_pos);
    mf_write(&channel, sizeof(channel_t));
    header.ch_count++;
    _writeHeader(header);
    return mf_commit();
}

/* Bank insert/delete reshapes the per-bank offset table; deferred until the
 * UI needs on-radio zone editing (channels + contacts cover the common case).
 * Returning -1 keeps the codeplug consistent rather than risking a malformed
 * offset table. */
int cps_insertBankHeader(bankHdr_t b_header, uint16_t pos) { (void)b_header; (void)pos; return -1; }
int cps_insertBankData(uint32_t ch, uint16_t bank_pos, uint16_t pos) { (void)ch; (void)bank_pos; (void)pos; return -1; }

/* ---- delete ---- */

static int mf_pullup(uint32_t offset, uint32_t amount)
{
    if(offset + amount > mf_size) return -1;
    memmove(mf_buf + offset, mf_buf + offset + amount, mf_size - offset - amount);
    mf_size -= amount;
    mf_dirty = true;
    return 0;
}

int cps_deleteContact(uint16_t pos)
{
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ct_count) return -1;
    uint32_t ct_pos = sizeof(cps_header_t) + pos * sizeof(contact_t);
    if(mf_pullup(ct_pos, sizeof(contact_t)) < 0) return -1;
    header.ct_count--;
    _writeHeader(header);
    _updateCtNumbering(pos, false);
    return mf_commit();
}

int cps_deleteChannel(channel_t channel, uint16_t pos)
{
    (void) channel;
    cps_header_t header = { 0 };
    if(_readHeader(&header)) return -1;
    if(pos >= header.ch_count) return -1;
    uint32_t ch_pos = sizeof(cps_header_t) + header.ct_count * sizeof(contact_t) +
                      pos * sizeof(channel_t);
    if(mf_pullup(ch_pos, sizeof(channel_t)) < 0) return -1;
    header.ch_count--;
    _writeHeader(header);
    return mf_commit();
}

int cps_deleteBankHeader(uint16_t pos) { (void)pos; return -1; }
int cps_deleteBankData(uint16_t bank_pos, uint16_t pos) { (void)bank_pos; (void)pos; return -1; }
