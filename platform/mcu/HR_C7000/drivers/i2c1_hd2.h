/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Radio-bus I2C1 for the HR_C7000 / ck803s SoC (Ailunce HD2).  The AT1846S
 * RF transceiver (slave 0x71 / 0xE2) sits on the on-chip DesignWare I2C1
 * controller (base 0x14070000, SCL=PTA7 / SDA=PTA8).
 *
 * The public surface mirrors the MK22FN512xxx12 I2C0 driver so chip-driver
 * code (AT1846S_HD2.cpp) can call the same i2c0_* entry points used on GDx.
 */

#ifndef I2C1_HD2_H
#define I2C1_HD2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void i2c0_init(void);
void i2c0_terminate(void);

void i2c0_write(uint8_t addr, void *buf, size_t len, bool sendStop);
void i2c0_read(uint8_t addr, void *buf, size_t len);

/* Address-presence probe: START + (addr & 0xFE) write byte, sample ACK, STOP.
 * Returns true iff a slave ACKed the address.  Used to confirm a chip is on the
 * bus without writing any register. */
bool i2c0_probe(uint8_t addr);

void i2c0_lockDeviceBlocking(void);
void i2c0_releaseDevice(void);

#ifdef __cplusplus
}
#endif

#endif /* I2C1_HD2_H */
