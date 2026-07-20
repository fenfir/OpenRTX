/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * HR_C7000 (Ailunce HD2) MMIO register map -- minimal bring-up subset.
 *
 * Only the registers the RTC / display / backlight / keyboard / power
 * drivers need are declared here.  The base addresses, pin-mux masks and
 * "magic" configuration words are hardware facts recovered by on-device
 * reverse engineering of the vendor V2.1.3 firmware; they are not guessed.
 * The wider modem / codec / RF register set is intentionally omitted from
 * this minimal port.
 */

#ifndef HD2_REGS_H
#define HD2_REGS_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 *  SOCSYS -- chip system control / pin-mux (base 0x11000000)
 * ------------------------------------------------------------------------- */
#define SOCSYS_BASE 0x11000000u
#define SOCSYS_REG(off) (*(volatile uint32_t *)(SOCSYS_BASE + (off)))

#define SOCSYS_IO_DIPLEX0 SOCSYS_REG(0x34u) /* PTA pad mux (i2c1/uart2/audio) */
#define SOCSYS_IO_DIPLEX1 SOCSYS_REG(0x38u) /* PTA pad mux (spi0 pins) */
#define SOCSYS_IO_DIPLEX2 SOCSYS_REG(0x3cu) /* PTC pad mux */

/*
 * PTC pad-mux values (DIPLEX2, manual 4.4.4.6 -- upper bits are write-only,
 * so always write the whole-register constant, never read-modify-write):
 *   PTC_GPIO: every PTC pad in GPIO mode (keypad matrix scan).
 *   LCD_I80:  bits 3..18 = 0 -> the HW i8080 controller @0x12000000 owns
 *             PTC3-6 (CS/RS/WR/RD) and PTC7-14 (DB0-7); the rest stays GPIO
 *             (PTC2 = panel reset, ADC/DAC pads).
 * The keypad matrix shares the LCD data pins (rows PTC7-10, cols PTC11-14),
 * so a scan momentarily swaps the mux GPIO<->I80 (see keyboard_HD2.c).
 */
#define HD2_DIPLEX2_PTC_GPIO 0x3ffffffbu
#define HD2_DIPLEX2_LCD_I80 0x3ff80003u

/* -------------------------------------------------------------------------
 *  GPIO banks -- DesignWare DW_apb_gpio
 * ------------------------------------------------------------------------- */
#define GPIOA_BASE 0x14020000u
#define GPIOB_BASE 0x14100000u
#define GPIOC_BASE 0x14110000u

#define GPIO_DR_OFF 0x00u       /* output data          */
#define GPIO_DDR_OFF 0x04u      /* direction: 1 = output */
#define GPIO_EXT_PORT_OFF 0x50u /* input read-back       */

#define GPIOA_REG(off) (*(volatile uint32_t *)(GPIOA_BASE + (off)))
#define GPIOB_REG(off) (*(volatile uint32_t *)(GPIOB_BASE + (off)))
#define GPIOC_REG(off) (*(volatile uint32_t *)(GPIOC_BASE + (off)))

#define GPIOA_DR GPIOA_REG(GPIO_DR_OFF)
#define GPIOA_DDR GPIOA_REG(GPIO_DDR_OFF)
#define GPIOB_DR GPIOB_REG(GPIO_DR_OFF)
#define GPIOB_DDR GPIOB_REG(GPIO_DDR_OFF)
#define GPIOB_EXT_PORT GPIOB_REG(GPIO_EXT_PORT_OFF)
#define GPIOC_DR GPIOC_REG(GPIO_DR_OFF)
#define GPIOC_DDR GPIOC_REG(GPIO_DDR_OFF)
#define GPIOC_EXT_PORT GPIOC_REG(GPIO_EXT_PORT_OFF)

/* GPIOB control/status bits (live-verified against V2.1.3). */
#define LED_GREEN_BIT (1u << 0) /* PTB0,  active-high (also miosix ledOn) */
#define LED_RED_BIT (1u << 1)   /* PTB1,  active-high */
#define PTT_BIT (1u << 11)      /* PTB11, PTT button, active-LOW */
#define PWR_KNOB_BIT (1u << 12) /* PTB12, volume/power knob: LOW = on */
#define PWR_HOLD_BIT (1u << 13) /* PTB13, power self-latch: HIGH = hold */
#define GPS_PWR_BIT (1u << 15)  /* PTB15, GPS module power: HIGH = on */

/* -------------------------------------------------------------------------
 *  LCD -- HR_C7000 hardware i8080 controller (base 0x12000000, manual 5.3)
 * ------------------------------------------------------------------------- */
#define LCD_BASE 0x12000000u
#define LCD_INDEX (*(volatile uint32_t *)(LCD_BASE + 0x00u)) /* command cycle */
#define LCD_DATA \
    (*(volatile uint32_t *)(LCD_BASE + 0x04u)) /* 8-bit data cycle */
#define LCD_WCFG \
    (*(volatile uint32_t *)(LCD_BASE + 0x10u)) /* write strobe timing */
#define LCD_WCFG_DEFAULT 0x020202u             /* 2/2/2 strobes ~143 ns */

#define LCD_RESET_BIT (1u << 2) /* panel reset on PTC2 (GPIO, active-low) */

/* -------------------------------------------------------------------------
 *  PWM block (base 0x140c0000).  Channel stride 0x20; ch0 = LCD backlight.
 *  Per-channel words: [0]=control, [1]=?, [2]=period, [3]=duty.
 * ------------------------------------------------------------------------- */
#define PWM_BASE 0x140c0000u
#define PWM_CH_STRIDE 0x20u
#define PWM_CH0_BASE ((volatile uint32_t *)(PWM_BASE + 0u * PWM_CH_STRIDE))
#define PWM_TIMER_HZ 42000000u /* post-PLL source feeding the PWM block */

/* PWM ch0 duty-gating control words: the V2.1.3 boot leaves all four at 0x300.
 * Left at 0 they silently squash the duty cycle -> the backlight LED stays dark
 * even with the PWM counter running. */
#define PWM_CH0_GATE0 (*(volatile uint32_t *)(PWM_BASE + 0x10u))
#define PWM_CH0_GATE1 (*(volatile uint32_t *)(PWM_BASE + 0x14u))
#define PWM_CH0_GATE2 (*(volatile uint32_t *)(PWM_BASE + 0x18u))
#define PWM_CH0_GATE3 (*(volatile uint32_t *)(PWM_BASE + 0x1cu))
#define PWM_CH0_GATE_ON 0x00000300u

/* -------------------------------------------------------------------------
 *  I2C2 -- DesignWare DW_apb_i2c, the RTC's dedicated internal bus.
 *  (base 0x14080000; RTC slave address 0x70).  The HR_C7000 adds a
 *  non-standard IC_START (+0xa0) trigger that must be written to launch a
 *  transfer -- a stock DW_apb_i2c starts implicitly.
 * ------------------------------------------------------------------------- */
#define I2C2_BASE 0x14080000u
#define I2C2_REG(off) (*(volatile uint32_t *)(I2C2_BASE + (off)))
#define I2C2_IC_CON I2C2_REG(0x00u)      /* master cfg = 0x65 */
#define I2C2_IC_TAR I2C2_REG(0x04u)      /* target slave 7-bit address */
#define I2C2_IC_DATA_CMD I2C2_REG(0x10u) /* tx data byte / rx command */
#define I2C2_IC_FS_SCL_HCNT I2C2_REG(0x1cu)
#define I2C2_IC_FS_SCL_LCNT I2C2_REG(0x20u)
#define I2C2_IC_INTR_MASK I2C2_REG(0x30u)
#define I2C2_IC_RX_TL I2C2_REG(0x38u)
#define I2C2_IC_TX_TL I2C2_REG(0x3cu)
#define I2C2_IC_CLR_TX_ABRT I2C2_REG(0x54u) /* read clears TX_ABRT latch */
#define I2C2_IC_ENABLE I2C2_REG(0x6cu)
#define I2C2_IC_STATUS I2C2_REG(0x70u)
#define I2C2_IC_TX_ABRT_SRC I2C2_REG(0x80u)
#define I2C2_IC_ENABLE_STATUS I2C2_REG(0x9cu) /* bit0: IC_EN settled */
#define I2C2_IC_START I2C2_REG(0xa0u) /* HR_C7000-specific transfer trigger */

#define I2C_CMD_READ 0x100u           /* IC_DATA_CMD bit8: read request */
#define I2C_CMD_STOP 0x200u           /* IC_DATA_CMD bit9: STOP after byte */
#define I2C_STA_ACTIVITY (1u << 0)    /* bus busy */
#define I2C_STA_TFNF (1u << 1)        /* tx FIFO not full */
#define I2C_STA_TFE (1u << 2)         /* tx FIFO empty */
#define I2C_STA_RFNE (1u << 3)        /* rx FIFO not empty */

#define I2C_CON_MASTER_FS 0x65u       /* master, 7-bit, fast-mode, restart-en */
/* 400 kHz off the 42 MHz APB clock (clk/400000 = 105, split hi/lo). */
#define I2C_SCL_HCNT 0x2cu
#define I2C_SCL_LCNT 0x34u

#define RTC_I2C_SLAVE 0x70u /* RTC device address (7'b1110000) */

/* -------------------------------------------------------------------------
 *  ADC -- HR_C7000 on-chip ADC (base 0x140d0000, manual 4.13).  The battery
 *  pack sits on channel 2 -> ADC_DATA_CD bits[9:0].
 * ------------------------------------------------------------------------- */
#define ADC_BASE 0x140d0000u
#define ADC_REG(off) (*(volatile uint32_t *)(ADC_BASE + (off)))
#define ADC_CTRL ADC_REG(0x00u)
#define ADC_INTR ADC_REG(0x08u)
#define ADC_START ADC_REG(0x14u)      /* write 1 to start a conversion */
#define ADC_CTRL_STATE ADC_REG(0x18u) /* bit0 BUSY, bits[5:1] SAMP_FSM */
#define ADC_CTRL_STOP ADC_REG(0x20u)
#define ADC_CH_VLD ADC_REG(0x24u)     /* channel-enable bitmask (1<<ch) */
#define ADC_SEOC_TIME ADC_REG(0x28u)
#define ADC_P2S_EN ADC_REG(0x2cu)
#define ADC_DATA_AB ADC_REG(0x30u) /* ch0 [9:0] / ch1 [25:16] */
#define ADC_DATA_CD ADC_REG(0x34u) /* ch2 (battery) / ch3 */
#define ADC_DATA_EF ADC_REG(0x38u) /* ch4 / ch5 */
#define ADC_DATA_GH ADC_REG(0x3cu) /* ch6 / ch7 */

#endif                             /* HD2_REGS_H */
