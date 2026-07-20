/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RTC driver for the Ailunce HD2 (HR_C7000 / CK803S), implementing
 * OpenRTX peripherals/rtc.h.
 *
 * The HR_C7000 RTC is an I2C timekeeper on the SoC's internal I2C2
 * DesignWare APB master (base 0x14080000, slave 0x70; manual 4.9).  Register
 * values are plain BINARY (not BCD): seconds/minutes are 6-bit, hours 5-bit,
 * and the day field is a 16-bit count of days since 1970-01-01.  The core is
 * battery-backed and keeps running across power cycles, so init only ensures
 * the run bit is set rather than replaying the cold first-power sequence.
 *
 * HR_C7000 I2C quirk (manual 5.1.6.27): unlike stock DesignWare, this master
 * does NOT auto-start when the TX FIFO has data -- it waits for an explicit
 * IC_START (+0xa0) write.  Live-verified.
 */

#include "peripherals/rtc.h"
#include "hd2_regs.h"

/* RTC register offsets (manual 4.9.4). */
#define RTC_VAL_S 0x00u
#define RTC_VAL_M 0x01u
#define RTC_VAL_H 0x02u
#define RTC_VAL_D_L 0x03u
#define RTC_VAL_D_H 0x04u
#define RTC_LOAD_S 0x0cu
#define RTC_LOAD_M 0x0du
#define RTC_LOAD_H 0x0eu
#define RTC_LOAD_D_L 0x0fu
#define RTC_LOAD_D_H 0x10u
#define RTC_CCR 0x11u
#define RTC_LOAD_VSTAT 0x12u

#define RTC_CCR_RUN (1u << 1)
#define RTC_CCR_LOAD (1u << 2)

#define I2C_SPIN_LIMIT 0x40000

static bool rtc_ready = false;

/* ---- DesignWare I2C2 master ---------------------------------------- */

#define I2C2_WAIT_IDLE()                                                     \
    do {                                                                     \
        int _s;                                                              \
        for (_s = I2C_SPIN_LIMIT; _s && (I2C2_IC_STATUS & I2C_STA_ACTIVITY); \
             --_s) {                                                         \
        }                                                                    \
    } while (0)

/*
 * Enable the controller ONCE and leave it enabled (target set while disabled,
 * then wait for IC_ENABLE_STATUS to settle), mirroring the proven i2c_csky
 * (I2C1) sequence on the same DesignWare core.  Toggling IC_ENABLE around every
 * transfer -- and not waiting for it to settle -- corrupts the FIFO/command
 * alignment and returns garbage (the original RTC round-trip failure).
 */
static void i2c2_setup(void)
{
    int spin;

    I2C2_IC_ENABLE = 0u; /* IC_TAR can only change while off */
    for (spin = I2C_SPIN_LIMIT; spin && (I2C2_IC_ENABLE_STATUS & 1u); --spin) {
    }

    I2C2_IC_CON = I2C_CON_MASTER_FS; /* master, 7-bit, FS, restart */
    I2C2_IC_FS_SCL_HCNT = I2C_SCL_HCNT;
    I2C2_IC_FS_SCL_LCNT = I2C_SCL_LCNT;
    I2C2_IC_INTR_MASK = 0x40u;
    I2C2_IC_RX_TL = 6u;
    I2C2_IC_TX_TL = 6u;
    I2C2_IC_TAR = RTC_I2C_SLAVE;

    I2C2_IC_ENABLE = 1u; /* enable once, keep enabled */
    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_ENABLE_STATUS & 1u); --spin) {
    }
}

/* Post-transfer health check: a clean transfer leaves the TX FIFO empty, the
 * bus idle and no abort latched.  Otherwise clear the abort and re-init. */
static void i2c2_recover(void)
{
    if ((I2C2_IC_STATUS & I2C_STA_TFE) && !(I2C2_IC_STATUS & I2C_STA_ACTIVITY)
        && (I2C2_IC_TX_ABRT_SRC == 0u))
        return;
    (void)I2C2_IC_CLR_TX_ABRT;
    i2c2_setup();
}

static void rtc_reg_write(uint8_t reg, uint8_t val)
{
    int spin;

    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_STATUS & I2C_STA_TFNF);
         --spin) {
    }
    I2C2_IC_DATA_CMD = reg;
    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_STATUS & I2C_STA_TFNF);
         --spin) {
    }
    I2C2_IC_DATA_CMD = (uint32_t)val | I2C_CMD_STOP;

    I2C2_IC_START = 1u; /* HR_C7000 explicit transfer trigger */
    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_STATUS & I2C_STA_TFE);
         --spin) {
    }
    I2C2_WAIT_IDLE();
    I2C2_IC_START = 0u;
    i2c2_recover();
}

static uint8_t rtc_reg_read(uint8_t reg)
{
    int spin;
    uint8_t val;

    /* Combined repeated-START read: write the register pointer (no STOP),
     * then a read command with STOP, all loaded before IC_START. */
    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_STATUS & I2C_STA_TFNF);
         --spin) {
    }
    I2C2_IC_DATA_CMD = reg;
    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_STATUS & I2C_STA_TFNF);
         --spin) {
    }
    I2C2_IC_DATA_CMD = I2C_CMD_READ | I2C_CMD_STOP;

    I2C2_IC_START = 1u;
    for (spin = I2C_SPIN_LIMIT; spin && !(I2C2_IC_STATUS & I2C_STA_RFNE);
         --spin) {
    }
    val = (uint8_t)I2C2_IC_DATA_CMD;

    I2C2_WAIT_IDLE();
    I2C2_IC_START = 0u;
    i2c2_recover();
    return val;
}

/* ---- civil-date <-> days-since-1970 (Howard Hinnant, branch-free) --- */

static int32_t civil_to_days(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void days_to_civil(int32_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t yr = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : -9u);
    *y = yr + (*m <= 2);
}

/* ---- OpenRTX peripherals/rtc.h interface --------------------------- */

void rtc_init()
{
    uint8_t ccr;

    i2c2_setup();
    ccr = rtc_reg_read(RTC_CCR);
    if ((ccr & RTC_CCR_RUN) == 0u)
        rtc_reg_write(RTC_CCR, ccr | RTC_CCR_RUN);

    rtc_ready = true;
}

void rtc_terminate()
{
    rtc_ready = false;
}

datetime_t rtc_getTime()
{
    datetime_t t = { 0 };
    uint8_t s, m, h, dl, dh;
    int32_t days, year;
    uint32_t mon, mday;

    if (!rtc_ready)
        rtc_init();

    s = rtc_reg_read(RTC_VAL_S);
    m = rtc_reg_read(RTC_VAL_M);
    h = rtc_reg_read(RTC_VAL_H);
    dl = rtc_reg_read(RTC_VAL_D_L);
    dh = rtc_reg_read(RTC_VAL_D_H);

    t.second = (int8_t)(s & 0x3fu);
    t.minute = (int8_t)(m & 0x3fu);
    t.hour = (int8_t)(h & 0x1fu);

    days = (int32_t)(((uint32_t)dh << 8) | (uint32_t)dl);
    days_to_civil(days, &year, &mon, &mday);
    t.date = (int8_t)mday;
    t.month = (int8_t)mon;
    t.year = (uint8_t)((year >= 2000) ? (year - 2000) : 0);

    /* 1970-01-01 was a Thursday; map to 1..7 (Mon..Sun). */
    t.day = (int8_t)((((days % 7) + 7 + 3) % 7) + 1);
    return t;
}

void rtc_setTime(datetime_t t)
{
    int32_t days;
    uint32_t day16;
    uint8_t ccr;

    if (!rtc_ready)
        rtc_init();

    days = civil_to_days(2000 + (int32_t)t.year, (uint32_t)t.month,
                         (uint32_t)t.date);
    if (days < 0)
        days = 0;
    day16 = (uint32_t)days & 0xffffu;

    rtc_reg_write(RTC_LOAD_VSTAT, 0u);
    rtc_reg_write(RTC_LOAD_H, (uint8_t)((uint32_t)t.hour & 0x1fu));
    rtc_reg_write(RTC_LOAD_M, (uint8_t)((uint32_t)t.minute & 0x3fu));
    rtc_reg_write(RTC_LOAD_S, (uint8_t)((uint32_t)t.second & 0x3fu));
    rtc_reg_write(RTC_LOAD_D_L, (uint8_t)(day16 & 0xffu));
    rtc_reg_write(RTC_LOAD_D_H, (uint8_t)((day16 >> 8) & 0xffu));
    rtc_reg_write(RTC_LOAD_VSTAT, 1u);

    ccr = rtc_reg_read(RTC_CCR);
    rtc_reg_write(RTC_CCR, ccr | RTC_CCR_LOAD | RTC_CCR_RUN);
}

void rtc_setHour(uint8_t hours, uint8_t minutes, uint8_t seconds)
{
    datetime_t t = rtc_getTime();
    t.hour = (int8_t)hours;
    t.minute = (int8_t)minutes;
    t.second = (int8_t)seconds;
    rtc_setTime(t);
}

void rtc_setDate(uint8_t date, uint8_t month, uint8_t year)
{
    datetime_t t = rtc_getTime();
    t.date = (int8_t)date;
    t.month = (int8_t)month;
    t.year = year;
    rtc_setTime(t);
}

void rtc_dstSet()
{
} /* no hardware DST bit; handled in higher layers */
void rtc_dstClear()
{
}
