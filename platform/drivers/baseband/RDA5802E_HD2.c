/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Broadcast-FM tuner driver for the Ailunce HD2 (HR_C7000).  See
 * RDA5802E_HD2.h for the chip overview.
 *
 * CHIP: RDA Microelectronics RDA5802E (single-chip broadcast-FM tuner),
 * confirmed against vendor/RDA5802E-RDA.pdf.  It sits on the AT1846S's GPIOA
 * bit-bang I2C bus at 8-bit write address 0x20 (7-bit 0x10) = the RDA5802E
 * *sequential* access address.  IMPORTANT consequences from the datasheet:
 *   - Registers are 16-bit, MSB-first on the wire.
 *   - At address 0x20 there is NO [reg][val] random access: every WRITE starts
 *     at register 0x02 and auto-increments; every READ starts at register 0x0A.
 *   - reg 0x02: bit15 DHIZ(1=output normal,0=high-Z), bit14 DMUTE(1=unmute),
 *     bit1 SOFT_RESET, bit0 ENABLE.  reg 0x03: CHAN[9:0]<<6 | TUNE(bit4) |
 *     BAND[3:2] | SPACE[1:0].  reg 0x05: SEEKTH/LNA/VOLUME[3:0].
 *   - read reg 0x0A: bit14 STC(tune complete), bit10 ST(stereo), READCHAN[9:0];
 *     reg 0x0B: RSSI[6:0] in bits15:9, bit8 FM_TRUE(station present), bit7 RDY.
 *
 * NOTE: scripts/labels/fm_broadcast.py mis-modelled this as 8-bit [reg][val]
 * writes with a 0x7f mask -- that interpretation is WRONG for an RDA5802E in
 * sequential mode (a 2-byte write only ever touches reg 0x02).  The vendor's
 * 64-byte "init burst" and 4-byte "tune burst" are simply sequential 16-bit
 * register writes starting at 0x02; we replay them as such.
 *
 * GPIO control (LIVE-VERIFIED 2026-06-08, before/after MMIO diff while playing
 * broadcast out the speaker): FM_ENABLE_BIT=PTB20 (LOW=active) and
 * AUDIO_ROUTE_BIT=PTB10 (LOW=audio->speaker).  See hd2_regs.h.
 */

#include "drivers/baseband/RDA5802E_HD2.h"

#include "drivers/i2c_csky.h"
#include "interfaces/delays.h"
#include "hd2_regs.h"

/* 8-bit write address (sequential mode) of the RDA5802E. */
#define FM_I2C_ADDR   0x20u

/*
 * Sequential register-write block, starting at RDA5802E register 0x02.  This is
 * the vendor's factory analog/AGC config (regs 0x02..0x21), read back LIVE from
 * the running firmware at flash 0x0307ade4 -- confirmed real flash content (not
 * LCSFC-overlaid) and build-stable; see tmp/fm_probe/init_block_0307ade4.txt.
 *
 * Decoded head: reg0x02=0xc601 (DHIZ=1,DMUTE=1,SEEKUP=1,ENABLE=1),
 * reg0x03=0x000a (BAND=10), reg0x05=0x88af (LNAP, VOLUME=max).  fm_broadcast.py
 * labelled byte[0]=0xc6 a "register" -- it is actually the high byte of reg0x02.
 */
static const uint8_t fm_init_seq[64] = {
    0xc6, 0x01, 0x00, 0x0a, 0x04, 0x00, 0x88, 0xaf, 0x00, 0x00, 0x5e, 0xc6,
    0x50, 0x96, 0x00, 0x00, 0x40, 0x00, 0x00, 0x8f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xf0, 0x05, 0x90, 0x00, 0xf4, 0x84, 0x70, 0x01,
    0x40, 0xf0, 0x21, 0x80, 0x7a, 0xd0, 0x3e, 0x40, 0x55, 0xa9, 0xe8, 0x48,
    0x50, 0x80, 0x00, 0x00, 0x37, 0xb6, 0x40, 0x0c, 0x07, 0x9b, 0x4c, 0x1d,
    0x81, 0x11, 0x45, 0xc0,
};

/* ------------------------------------------------------------------ *
 *  Low-level RDA5802E I2C (sequential mode, address 0x20)
 * ------------------------------------------------------------------ */

/* Sequential write: bytes are reg0x02-hi, reg0x02-lo, reg0x03-hi, ... (the chip
 * auto-increments the register pointer).  Always starts at reg 0x02. */
static void fm_seq_write(const uint8_t *buf, size_t len)
{
    i2c0_lockDeviceBlocking();
    i2c0_write(FM_I2C_ADDR, (void *)buf, len, true);
    i2c0_releaseDevice();
}

/* Write registers 0x02 and 0x03 in one transaction (the common case: set the
 * config word + channel/tune). */
static void fm_write_02_03(uint16_t reg02, uint16_t reg03)
{
    uint8_t b[4] = {
        (uint8_t)(reg02 >> 8), (uint8_t)reg02,
        (uint8_t)(reg03 >> 8), (uint8_t)reg03,
    };
    fm_seq_write(b, sizeof b);
}

/* ------------------------------------------------------------------ *
 *  Public API
 * ------------------------------------------------------------------ */

void fm_broadcast_init(void)
{
    i2c0_init();

    /* PTB20 (enable) + PTB10 (route) outputs, parked in standby (both HIGH). */
    gpio_atomic_set(&GPIOB_DDR, FM_ENABLE_BIT | AUDIO_ROUTE_BIT);
    gpio_atomic_set(&GPIOB_DR,  FM_ENABLE_BIT | AUDIO_ROUTE_BIT);
}

void fm_broadcast_powerup(void)
{
    fm_broadcast_init();

    /* Board-level enable on PTB20.  Live running state (audio playing) is LOW;
     * vendor power-up pulses it before talking to the chip.  Pulse HIGH (reset)
     * then settle LOW (active) for I2C. */
    gpio_atomic_set(&GPIOB_DR, FM_ENABLE_BIT);    /* PTB20 HIGH (reset pulse) */
    delayUs(10);
    gpio_atomic_clear(&GPIOB_DR, FM_ENABLE_BIT);  /* PTB20 LOW  (active)      */
    delayMs(1);

    /* Sequential write of the factory config block (regs 0x02..): leaves the
     * chip ENABLEd, output non-high-Z, unmuted, VOLUME=max, BAND=10 (76-108). */
    fm_seq_write(fm_init_seq, sizeof fm_init_seq);
    delayMs(2);
}

uint16_t fm_broadcast_tune(uint32_t freq_khz)
{
    if (freq_khz < FM_BCAST_MIN_KHZ) freq_khz = FM_BCAST_MIN_KHZ;
    if (freq_khz > FM_BCAST_MAX_KHZ) freq_khz = FM_BCAST_MAX_KHZ;

    /* channel = (f - 76.0 MHz) / 100 kHz  (RDA5802E BAND=10, SPACE=00). */
    uint16_t chan = (uint16_t)((freq_khz - FM_BCAST_MIN_KHZ) / FM_BCAST_STEP_KHZ);

    /* reg 0x02 = 0xc201: DHIZ=1, DMUTE=1, SEEKUP=1, ENABLE=1.
     * reg 0x03 = CHAN<<6 | TUNE(bit4) | BAND=10(bits3:2) | SPACE=00.
     * (Bit-identical to the vendor's {0xc2,0x01,chan>>2,((chan&3)<<6)|0x18}.) */
    uint16_t reg03 = (uint16_t)((chan << 6) | 0x0010u | (0x2u << 2));
    fm_write_02_03(0xc201u, reg03);

    return chan;
}

bool fm_broadcast_status(bool *tuned, uint16_t *channel)
{
    /* Sequential read starts at reg 0x0A: byte0/1 = reg0x0A hi/lo, byte2/3 =
     * reg0x0B hi/lo.  STC = reg0x0A bit14 (byte0 bit6); READCHAN = reg0x0A[9:0];
     * RSSI = reg0x0B[15:9] (byte2[7:1]); FM_TRUE = reg0x0B bit8 (byte2 bit0). */
    uint8_t b[4] = { 0 };
    i2c0_lockDeviceBlocking();
    i2c0_read(FM_I2C_ADDR, b, sizeof b);
    i2c0_releaseDevice();

    if (tuned)   *tuned   = (b[0] & 0x40u) != 0u;            /* STC */
    if (channel) *channel = (uint16_t)(((b[0] & 0x03u) << 8) | b[1]);
    return true;
}

/* RSSI[6:0] from read reg 0x0B, for the UI level meter.  0 on a bad read. */
uint8_t fm_broadcast_rssi(void)
{
    uint8_t b[4] = { 0 };
    i2c0_lockDeviceBlocking();
    i2c0_read(FM_I2C_ADDR, b, sizeof b);
    i2c0_releaseDevice();
    return (uint8_t)(b[2] >> 1);    /* reg0x0B[15:9] */
}

void fm_broadcast_route_speaker(void)
{
    /* §D (LIVE-verified speaker path): PTB10 LOW routes broadcast audio to the
     * speaker; un-mute the speaker amp (PTB4 active-low -> LOW). */
    gpio_atomic_clear(&GPIOB_DR, AUDIO_ROUTE_BIT);   /* PTB10 LOW  = routed    */
    gpio_atomic_clear(&GPIOB_DR, SPKR_AMP_BIT);      /* PTB4  LOW  = amp on    */
    gpio_atomic_set(&GPIOB_DR,   SPKR_GAIN_BIT);     /* PTB17 HIGH = full gain */
}

void fm_broadcast_powerdown(void)
{
    /* Disable the chip (reg0x02 ENABLE=0, matches vendor teardown 0xc200), then
     * hand audio/route back to the 2-way path (PTB10/PTB20 HIGH = standby). */
    fm_write_02_03(0xc200u, 0x0000u);
    gpio_atomic_set(&GPIOB_DR, AUDIO_ROUTE_BIT);     /* PTB10 HIGH = route off */
    gpio_atomic_set(&GPIOB_DR, FM_ENABLE_BIT);       /* PTB20 HIGH = standby   */
}
