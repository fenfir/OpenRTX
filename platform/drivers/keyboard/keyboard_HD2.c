/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HD2 keypad driver -- implements OpenRTX keyboard.h (kbd_init /
 * kbd_terminate / kbd_getKeys).
 *
 * The 4x4 matrix shares the LCD data pins on GPIOC (rows PTC7-10, cols
 * PTC11-14).  A scan momentarily swaps the PTC pad mux (DIPLEX2) from the
 * i8080 LCD controller to GPIO and back, and saves/restores GPIOC DR+DDR,
 * so the LCD bus is preserved across calls.  Rows sense through the
 * vendor-custom "alt-func B" selector at GPIOC+0x78 (set once in kbd_init).
 *
 * Because the matrix and the LCD share pins, a scan must NOT interleave
 * with a display_render(); OpenRTX runs the UI on a single thread that
 * scans then renders, so they never overlap.
 *
 * Matrix cell map, side keys (GPIOB.9 = SK1, GPIOB.7 = EMER) and the SK2
 * "row0 in every column" phantom are live-verified on the V2.1.3 hardware.
 * The rotary-encoder lines (GPIOA.0/1) are a first-cut poll and may need
 * revisiting.
 */

#include "interfaces/keyboard.h"
#include "hd2_regs.h"
#include <stdint.h>

#define GPIO_DR(base) (*(volatile uint32_t *)((base) + 0x00u))
#define GPIO_DDR(base) (*(volatile uint32_t *)((base) + 0x04u))
#define GPIO_EXT_PORT(base) (*(volatile uint32_t *)((base) + 0x50u))
#define GPIO_ALT_B(base) (*(volatile uint32_t *)((base) + 0x78u))

/* Matrix pins on GPIOC. */
#define KBD_ROW_SHIFT 7u
#define KBD_ROW_MASK (0xfu << KBD_ROW_SHIFT) /* bits 7..10 */
#define KBD_ROW_COUNT 4u
#define KBD_COL_SHIFT 11u
#define KBD_COL_MASK (0xfu << KBD_COL_SHIFT) /* bits 11..14 */
#define KBD_COL_COUNT 4u

/* Direct side keys on GPIOB (active-low). */
#define KBD_SIDE1_BIT (1u << 9) /* SK1  -> KEY_F1 */
#define KBD_EMER_BIT (1u << 7)  /* EMER -> KEY_F2 */

/* SK2 grounds the row0 sense line, so a scan reads row0 low in EVERY
 * column (bit col*4+row0 for all 4 cols).  Unique -> report KEY_F3. */
#define KBD_ROW0_ALL_COLS 0x1111u

/* Rotary quadrature lines on GPIOA (poll-mode first cut). */
#define KBD_ROT_A_BIT (1u << 0)
#define KBD_ROT_B_BIT (1u << 1)

/* Matrix (row, col) -> key.  Live-verified: the matrix column is the
 * physical key column {1,2,3}/{4,5,6}/{7,8,9}, col3 = nav keys. */
static const keyboard_t keymap[KBD_ROW_COUNT][KBD_COL_COUNT] = {
    { KEY_1, KEY_4, KEY_7, KEY_ENTER },     /* row0 */
    { KEY_2, KEY_5, KEY_8, KEY_UP },        /* row1 */
    { KEY_3, KEY_6, KEY_9, KEY_DOWN },      /* row2 */
    { KEY_STAR, KEY_0, KEY_HASH, KEY_ESC }, /* row3 */
};

static uint8_t rot_prev = 0;

static inline void scan_settle(void)
{
    for (volatile uint32_t i = 0; i < 200u; ++i) {
    }
}

void kbd_init()
{
    /* Route the row pins (GPIOC 7..10) to the alt-func-B keypad sense path;
     * without this they read 0 (they are LCD-bus outputs otherwise). */
    GPIO_ALT_B(GPIOC_BASE) |= KBD_ROW_MASK;

    /* Side keys + rotary are inputs. */
    GPIO_DDR(GPIOB_BASE) &= ~(KBD_SIDE1_BIT | KBD_EMER_BIT);
    GPIO_DDR(GPIOA_BASE) &= ~(KBD_ROT_A_BIT | KBD_ROT_B_BIT);

    uint32_t a = GPIO_EXT_PORT(GPIOA_BASE);
    rot_prev = (uint8_t)(((a & KBD_ROT_A_BIT) ? 1u : 0u)
                         | ((a & KBD_ROT_B_BIT) ? 2u : 0u));
}

void kbd_terminate()
{
    /* DR/DDR are restored per scan; nothing to release. */
}

/* Raw matrix scan: 4 nibbles (one per col), each bit = one row read.
 * bit=1 -> row high (no key); bit=0 -> key pressed in that (row, col). */
static uint16_t scan_matrix(void)
{
    uint32_t saved_dr = GPIO_DR(GPIOC_BASE);
    uint32_t saved_ddr = GPIO_DDR(GPIOC_BASE);

    /* Borrow the shared PTC pads from the i8080 LCD controller. */
    SOCSYS_IO_DIPLEX2 = HD2_DIPLEX2_PTC_GPIO;

    /* rows (7..10) input, cols (11..14) output */
    GPIO_DDR(GPIOC_BASE) = (saved_ddr & ~KBD_ROW_MASK) | KBD_COL_MASK;

    uint16_t out = 0;
    for (unsigned col = 0; col < KBD_COL_COUNT; ++col) {
        uint32_t col_bit = 1u << (KBD_COL_SHIFT + col);
        GPIO_DR(GPIOC_BASE) = (saved_dr | KBD_COL_MASK) & ~col_bit;
        scan_settle();
        uint32_t rows = (GPIO_EXT_PORT(GPIOC_BASE) >> KBD_ROW_SHIFT) & 0xfu;
        out |= (uint16_t)(rows << (col * 4));
    }

    GPIO_DR(GPIOC_BASE) = saved_dr;
    GPIO_DDR(GPIOC_BASE) = saved_ddr;
    SOCSYS_IO_DIPLEX2 = HD2_DIPLEX2_LCD_I80; /* hand pads back to the LCD */
    return out;
}

static keyboard_t rotary_step(void)
{
    uint32_t a = GPIO_EXT_PORT(GPIOA_BASE);
    uint8_t now = (uint8_t)(((a & KBD_ROT_A_BIT) ? 1u : 0u)
                            | ((a & KBD_ROT_B_BIT) ? 2u : 0u));
    uint8_t prev = rot_prev;
    rot_prev = now;

    static const int8_t qdec[16] = {
        0, +1, -1, 0, -1, 0, 0, +1, +1, 0, 0, -1, 0, -1, +1, 0,
    };
    int8_t step = qdec[(prev << 2) | now];
    if (step > 0)
        return KNOB_RIGHT;
    if (step < 0)
        return KNOB_LEFT;
    return 0;
}

keyboard_t kbd_getKeys()
{
    keyboard_t keys = 0;

    uint16_t raw = scan_matrix();
    if (raw != 0 && (raw & KBD_ROW0_ALL_COLS) == 0u) {
        keys |= KEY_F3; /* SK2: row0 low in every column */
    } else if (raw != 0) {
        for (unsigned col = 0; col < KBD_COL_COUNT; ++col) {
            uint8_t rows_high = (raw >> (col * 4)) & 0xfu;
            for (unsigned row = 0; row < KBD_ROW_COUNT; ++row)
                if ((rows_high & (1u << row)) == 0u) /* active-low */
                    keys |= keymap[row][col];
        }
    }

    uint32_t b = GPIO_EXT_PORT(GPIOB_BASE);
    if ((b & KBD_SIDE1_BIT) == 0u)
        keys |= KEY_F1; /* SK1  */
    if ((b & KBD_EMER_BIT) == 0u)
        keys |= KEY_F2; /* EMER */

    keys |= rotary_step();
    return keys;
}
