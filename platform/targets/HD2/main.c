/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HD2 OpenRTX bring-up main with a dynamic peek/poke loader.  Saves
 * re-flash cycles -- once this build is on the radio we can iterate
 * on chip-init experiments by writing MMIO over UART instead of
 * editing C and re-flashing.
 *
 * Boot sequence:
 *   1. platform_init  (V2.1.3 mmio mirror)
 *   2. boot beep
 *   3. PWM ch0 backlight at 76% duty
 *   4. display_init + green framebuffer fill + render
 *   5. enter loader loop
 *
 * Loader binary protocol (over UART0 @ 57600 8N1):
 *   'R' <addr32 LE> <size:1>  -> reply: <size> bytes  (read MMIO)
 *   'W' <addr32 LE> <val32 LE> -> reply: 'k'           (write u32 MMIO)
 *   'Z'                       -> jump to reset vector (DFU dance)
 *   'P'                       -> reply: 4 bytes "RTX1" (probe / sync)
 *   '?'                       -> ASCII help banner
 *   any other byte            -> ignored, no reply
 *
 * Multi-byte commands read greedily with no inter-byte timeout.  Host
 * is responsible for sending args back-to-back after the command byte.
 */

#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/display.h"
#include "interfaces/keyboard.h"
#include "interfaces/nvmem.h"
#include "drivers/SPI/spi_hd2.h"
#include "drivers/NVM/W25Qx.h"
#include "peripherals/spi.h"
#include "peripherals/gpio.h"
#include "drivers/gpio_csky.h"
#include "drivers/backlight/backlight.h"
#include "drivers/SPI/spi_hd2.h"
#include "drivers/NVM/W25Qx.h"
#include "build_version.h"
#include "hwconfig.h"
#include "loader_cmd.h"
#include "hd2_router.h"
#include "drivers/rtc_hd2.h"
#include "drivers/GPS/gps_HD2.h"
#include "peripherals/spi.h"
#include "peripherals/gpio.h"
#include "drivers/gpio_csky.h"
#include "hd2_regs.h"
#include "drivers/i2c_csky.h"
#include "drivers/baseband/fm_broadcast_HD2.h"
#include <stdint.h>
#include <string.h>

extern uint16_t *hd2_get_framebuffer(void);
extern uint8_t   hd2_backlight_last_level(void);
extern uint16_t  hd2_kbd_scan_raw(void);

/* UART0 (console), UART2 (GPS), GPIOB, SOCSYS, PWM regs from hd2_regs.h.
 * UART2 (0x14050000) = HD2-GPS NMEA module, 9600 8N1 (see drivers/GPS/gps_HD2.c). */
#define RED_BIT             LED_RED_BIT

static void putc_(char c)
{
    while ((UART0_LSR & LSR_TX_RDY) == 0) { }
    UART0_THR = (unsigned char)c;
}
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void put_hex32(uint32_t v)
{
    for (int i = 7; i >= 0; --i) {
        unsigned int n = (v >> (i * 4)) & 0xfu;
        putc_((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

static void put_hex8(uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    putc_(hex[(v >> 4) & 0xfu]);
    putc_(hex[v & 0xfu]);
}

static uint8_t uart_getb(void)
{
    while ((UART0_LSR & LSR_RX_RDY) == 0) { }
    return (uint8_t)(UART0_RBR & 0xffu);
}
static int uart_peekb(void)
{
    return (UART0_LSR & LSR_RX_RDY) ? (int)(UART0_RBR & 0xffu) : -1;
}

static void red_on(void)  { GPIOB_DR |= (1u << RED_BIT); }
static void red_off(void) { GPIOB_DR &= ~(1u << RED_BIT); }

/* ---- key-echo mode ('y' toggles) ----------------------------------- *
 * When enabled, the loader idle tick polls kbd_getKeys() and prints the
 * human-readable name of each key as it goes down/up, until 'y' is sent
 * again.  Handy for live keymap confirmation without a host poller. */
static int        g_keyecho      = 0;
static keyboard_t g_keyecho_prev = 0;

static const struct { keyboard_t bit; const char *name; } KEY_NAMES[] = {
    { KEY_1, "1" }, { KEY_2, "2" }, { KEY_3, "3" }, { KEY_4, "4" },
    { KEY_5, "5" }, { KEY_6, "6" }, { KEY_7, "7" }, { KEY_8, "8" },
    { KEY_9, "9" }, { KEY_0, "0" }, { KEY_STAR, "*" }, { KEY_HASH, "#" },
    { KEY_ENTER, "MENU" }, { KEY_ESC, "EXIT" }, { KEY_UP, "UP" },
    { KEY_DOWN, "DOWN" }, { KEY_LEFT, "LEFT" }, { KEY_RIGHT, "RIGHT" },
    { KEY_MONI, "MONI" }, { KEY_F1, "SK1" }, { KEY_F2, "EMER" },
    { KEY_F3, "SK2" }, { KEY_F4, "F4" }, { KEY_VOLUP, "VOL+" },
    { KEY_VOLDOWN, "VOL-" }, { KNOB_LEFT, "KNOB_L" }, { KNOB_RIGHT, "KNOB_R" },
};

static void keyecho_tick(void)
{
    if (!g_keyecho) return;
    keyboard_t k = kbd_getKeys();
    if (k == g_keyecho_prev) return;
    g_keyecho_prev = k;
    puts_("KEYS:");
    if (k == 0) {
        puts_(" (none)");
    } else {
        for (unsigned i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); ++i) {
            if (k & KEY_NAMES[i].bit) { putc_(' '); puts_(KEY_NAMES[i].name); }
        }
    }
    puts_("\r\n");
}

static void pwm_start(volatile uint32_t *p, unsigned freq_hz, unsigned duty_per_10000)
{
    p[0] &= ~1u;
    p[0] &= ~4u;
    p[0] &= ~8u;
    p[0] &= ~0x30u;
    p[0] |=  0x20u;
    p[0] |=  0x100u;
    p[0] |=  0x200u;
    p[1] = 5;
    p[2] = PWM_TIMER_HZ / freq_hz;
    p[3] = (p[2] * duty_per_10000) / 10000u;
    p[0] |= 4u;
    p[0] |= 1u;
}
static void pwm_stop(volatile uint32_t *p)
{
    p[0] &= ~1u;
    p[0] |=  2u;
}

static void boot_beep(void)
{
    SOCSYS_IO_DIPLEX0 &= ~DIPLEX0_AUDIO_MUTE;
    pwm_start(PWM_CH1_BASE, 1000u, 5000u);
    sleepFor(0u, 200u);
    pwm_stop(PWM_CH1_BASE);
    SOCSYS_IO_DIPLEX0 |= DIPLEX0_AUDIO_MUTE;
}

__attribute__((noreturn))
static void jump_to_iap(void)
{
    /* No wait -- user holds PTT+S1 BEFORE sending Z. */
    puts_("\r\n[JUMP]\r\n");
    for (volatile int i = 0; i < 100000; ++i) { }

    /* Jump to BOOTROM 0x00000004 -- the silicon mask-ROM reset handler.
     * This clears r0..r15, writes a fresh PSR (0x80000100), reinitialises
     * UART0 at 115200, then runs the normal flash-boot path: magic check
     * at 0x03000000 ("thd\\x0c") -> jump to stage-1 stub -> stage-1's
     * GPIO key-detect -> IAP-or-app decision.  This is closer to a true
     * power-on reset than jumping directly to the stage-1 stub at
     * 0x03000000, because the stage-1 stub assumes registers/PSR were
     * cleared by the BOOTROM and can panic if we hand it dirty state.
     * See docs/boot_flow.md section 1.1.  Peripherals (PWM, GPIO DRs)
     * are NOT reset by this path -- only the CPU register file.        */
    __asm__ volatile (
        "lrw  r0, 0x00050000\n\t"
        "mov  r14, r0\n\t"
        "lrw  r0, 0x00000004\n\t"
        "jmp  r0\n\t"
        : : : "r0", "r14"
    );
    while (1) { }
}

static const char *HELP =
    "\r\n--- HD2 dynamic loader ---\r\n"
    "  R <addr u32 LE> <size u8>           read MMIO  -> reply <size> bytes\r\n"
    "  r <addr u32 LE>                     WORD read u32 (full 32-bit; R truncates word-only regs)\r\n"
    "  W <addr u32 LE> <val  u32 LE>       write u32  -> reply 'k'\r\n"
    "  Z                                   jump to reset vector (DFU)\r\n"
    "  P                                   probe -> reply 'RTX1'\r\n"
    "  G                                   cr<0..15> + r28 cur + r28 boot (72 B)\r\n"
    "  T <secs u32 LE>                     set backlight inactivity timeout (0=always on)\r\n"
    "  B <0|1 u8>                          backlight off/on (re-kick)\r\n"
    "  K                                   kbd_getKeys() -> 4B LE keyboard_t\r\n"
    "  k                                   raw matrix scan -> 2B LE (4 nibbles, 1=row-high)\r\n"
    "  y                                   toggle key-echo: stream 'KEYS: <names>' until resent\r\n"
    "  J                                   read W25Q JEDEC-ID -> reply 3 hex bytes (e.g. 'ef4020')\r\n"
    "  b <freq u16 LE>                     play tone via platform_beepStart for 300ms (0=1kHz)\r\n"
    "  t                                   getTick check: print tick, delay 1s, print tick+delta\r\n"
    "  i                                   periodic-IRQ test: VBR+PIC+Timer2@100Hz, ISR counter over 1s\r\n"
    "  a                                   AT1846S RX self-test -> 'AT1846S reg33=.. rssi=.. reg30=.. rssi2=..'\r\n"
    "  M <addr u32 LE> <val u8>            single BYTE write (st.b; for codec @ 0x16000900)\r\n"
    "  q <reg u8>                          read AT1846S register -> 2B LE (run 'a' first)\r\n"
    "  Q <reg u8> <val u16 LE>             write AT1846S register -> ack 'k'\r\n"
    "  A                                   start OpenRTX UI superloop (loader stays live)\r\n"
    "  F                                   force DFU: jump to IAP 0x03201000 (no key combo)\r\n"
    "  N <src u8> <sink u8>                connect audio path -> reply u8 (0 ok / 0xff fail)\r\n"
    "  n <src u8> <sink u8>                disconnect audio path -> reply u8\r\n"
    "  S <target u8> <val u32 LE>          set routing target -> reply u32 LE readback (ffffffff=refused)\r\n"
    "  E                                   arm guarded routing ops (PA/TX/pwr-hold), one-shot\r\n"
    "  U                                   routing snapshot (DIPLEX/voice/af/codec/amp/AT1846S)\r\n"
    "  w <addr u8> <n u8> <bytes..>        bit-bang I2C write n bytes to 8-bit write addr (e.g. 0x20=FM) -> 'k'\r\n"
    "  p <addr u8>                         probe I2C addr (8-bit write addr) -> u8 0x00 ACK(present)/0xff NACK\r\n"
    "  f <freq_kHz u32 LE>                 FM-broadcast: powerup + tune(kHz) + route->speaker -> 'k' (0=97700)\r\n"
    "  ?                                   this help\r\n"
    "---------------------------\r\n";

/* ---- backlight / inactivity tracking -----------------------------
 *
 * Driver-level PWM ops live in platform/drivers/backlight/backlight_HD2.c
 * (matches OpenRTX backlight.h + display_setBacklightLevel API).  This
 * file just owns the "last-set level" + inactivity timer state. */
#define BACKLIGHT_DEFAULT_LEVEL 76u             /* matches vendor idle duty */

static uint32_t backlight_timeout_ms = 30000u;  /* default 30 s */
static uint32_t inactivity_ms        = 0u;
static uint8_t  bl_active_level      = BACKLIGHT_DEFAULT_LEVEL;

static void backlight_enable(void)
{
    display_setBacklightLevel(bl_active_level);
}

static void backlight_disable(void)
{
    display_setBacklightLevel(0u);
}

static void activity_kick(void)
{
    inactivity_ms = 0;
    if (hd2_backlight_last_level() == 0u) {
        display_setBacklightLevel(bl_active_level);
    }
}

/* ---- loader command module (see loader_cmd.h) ---------------------- *
 * Dispatch one command byte.  Reusable from both the bare loader loop
 * and the OpenRTX app superloop so probe/MMIO access stays live while
 * the UI runs. */
void loader_cmd_banner(void)
{
    puts_(HELP);
}

void loader_cmd_dispatch(uint8_t c)
{
    switch (c) {
        case 'P':
            putc_('R'); putc_('T'); putc_('X'); putc_('1');
            break;

        case 'R': {
            uint32_t a = (uint32_t)uart_getb();
            a |= (uint32_t)uart_getb() <<  8;
            a |= (uint32_t)uart_getb() << 16;
            a |= (uint32_t)uart_getb() << 24;
            uint8_t n = uart_getb();
            volatile const uint8_t *p = (volatile const uint8_t *)(uintptr_t)a;
            for (unsigned i = 0; i < n; ++i) {
                putc_((char)p[i]);
            }
            break;
        }

        case 'W': {
            uint32_t a = (uint32_t)uart_getb();
            a |= (uint32_t)uart_getb() <<  8;
            a |= (uint32_t)uart_getb() << 16;
            a |= (uint32_t)uart_getb() << 24;
            uint32_t v = (uint32_t)uart_getb();
            v |= (uint32_t)uart_getb() <<  8;
            v |= (uint32_t)uart_getb() << 16;
            v |= (uint32_t)uart_getb() << 24;
            *(volatile uint32_t *)(uintptr_t)a = v;
            putc_('k');
            break;
        }

        case 'M': {
            /* M <addr u32 LE> <val u8>  -- single BYTE write (st.b).  Needed
             * for the HR_C7000 codec block @ 0x16000900, which ignores word
             * writes; lets the host live-poke codec regs (DAC gain, ADC/mux)
             * without a reflash.  ack 'k'. */
            uint32_t a = (uint32_t)uart_getb();
            a |= (uint32_t)uart_getb() <<  8;
            a |= (uint32_t)uart_getb() << 16;
            a |= (uint32_t)uart_getb() << 24;
            uint8_t v = uart_getb();
            *(volatile uint8_t *)(uintptr_t)a = v;
            putc_('k');
            break;
        }

        case 'Z':
            jump_to_iap();
            break;

        case 'T': {
            /* Set backlight inactivity timeout (u32 LE seconds, 0 = disabled). */
            uint32_t s = (uint32_t)uart_getb();
            s |= (uint32_t)uart_getb() <<  8;
            s |= (uint32_t)uart_getb() << 16;
            s |= (uint32_t)uart_getb() << 24;
            backlight_timeout_ms = s * 1000u;
            inactivity_ms = 0u;
            putc_('k');
            break;
        }

        case 'B': {
            /* B <level u8>  -- 0..100 brightness; 0 = off, >0 sets active level. */
            uint8_t lvl = uart_getb();
            if (lvl > 100u) lvl = 100u;
            if (lvl != 0u) bl_active_level = lvl;
            display_setBacklightLevel(lvl);
            putc_('k');
            break;
        }

        case 'r': {
            /* r <addr u32 LE> -> reply 4 bytes LE (WORD read).
             * 'R' reads byte-by-byte, which truncates word-only registers
             * (socsys 0x11000000, some peripheral regs) to their low byte.
             * This does a single 32-bit load so the full value comes back. */
            uint32_t a = (uint32_t)uart_getb();
            a |= (uint32_t)uart_getb() <<  8;
            a |= (uint32_t)uart_getb() << 16;
            a |= (uint32_t)uart_getb() << 24;
            uint32_t v = *(volatile uint32_t *)(uintptr_t)a;
            putc_((char)(v & 0xffu));
            putc_((char)((v >>  8) & 0xffu));
            putc_((char)((v >> 16) & 0xffu));
            putc_((char)((v >> 24) & 0xffu));
            break;
        }

        case 'K': {
            /* K  -- kbd_getKeys() -> 4 bytes LE.
             * Hits the full driver including side keys + rotary. */
            uint32_t k = (uint32_t)kbd_getKeys();
            putc_((char)(k & 0xffu));
            putc_((char)((k >>  8) & 0xffu));
            putc_((char)((k >> 16) & 0xffu));
            putc_((char)((k >> 24) & 0xffu));
            break;
        }

        case 'k': {
            /* k  -- raw matrix scan -> 2 bytes LE.  Each nibble = the
             * 4 row reads with one col strobed low.  1 = row high
             * (no key), 0 = row low (key pressed in that cell). */
            uint16_t raw = hd2_kbd_scan_raw();
            putc_((char)(raw & 0xffu));
            putc_((char)((raw >> 8) & 0xffu));
            break;
        }

        case 'y': {
            /* y  -- toggle key-echo mode.  While on, the idle tick prints
             * "KEYS: <names>" on every change until 'y' is sent again. */
            g_keyecho = !g_keyecho;
            g_keyecho_prev = 0;
            puts_(g_keyecho ? "key-echo ON (send 'y' again to stop)\r\n"
                            : "key-echo OFF\r\n");
            break;
        }

        case 'G': {
            /* Dump CSKY V2 control registers cr<0..15,0> + r28 current +
             * r28 at boot (saved in startup.S to 0x0004fff8).
             * Total reply: 18 * 4 = 72 bytes. */
            uint32_t v;
            #define EMIT_U32(x) do { \
                uint32_t _v = (x); \
                putc_((char)(_v & 0xffu)); \
                putc_((char)((_v >>  8) & 0xffu)); \
                putc_((char)((_v >> 16) & 0xffu)); \
                putc_((char)((_v >> 24) & 0xffu)); \
            } while(0)
            #define DUMP_CR(N) do { \
                __asm__ volatile ("mfcr %0, cr<" #N ", 0>" : "=r"(v)); \
                EMIT_U32(v); \
            } while(0)
            DUMP_CR(0);  DUMP_CR(1);  DUMP_CR(2);  DUMP_CR(3);
            DUMP_CR(4);  DUMP_CR(5);  DUMP_CR(6);  DUMP_CR(7);
            DUMP_CR(8);  DUMP_CR(9);  DUMP_CR(10); DUMP_CR(11);
            DUMP_CR(12); DUMP_CR(13); DUMP_CR(14); DUMP_CR(15);
            __asm__ volatile ("mov %0, r28" : "=r"(v));
            EMIT_U32(v);                                /* r28 current */
            EMIT_U32(*(volatile uint32_t *)0x0004fff8u); /* r28 at boot */
            #undef EMIT_U32
            #undef DUMP_CR
            break;
        }

        case 'D': {
            /* D -- dump UART0 baud config so we can read the IAP's
             * actual divisor.  Reply: 12 hex chars + CRLF for the
             * three 16-bit values DLL, DLH, LCR (DLAB toggled to
             * expose DLL/DLH, then restored). */
            uint32_t lcr_saved = UART0_LCR;
            UART0_LCR = lcr_saved | LCR_DLAB;    /* DLAB=1 */
            uint32_t dll = UART0_DLL;
            uint32_t dlh = UART0_DLH;
            UART0_LCR = lcr_saved;               /* restore */
            /* emit "DLL=XXXX DLH=XXXX LCR=XXXX\r\n" */
            puts_("DLL=");
            putc_("0123456789abcdef"[(dll >> 12) & 0xfu]);
            putc_("0123456789abcdef"[(dll >>  8) & 0xfu]);
            putc_("0123456789abcdef"[(dll >>  4) & 0xfu]);
            putc_("0123456789abcdef"[(dll      ) & 0xfu]);
            puts_(" DLH=");
            putc_("0123456789abcdef"[(dlh >> 12) & 0xfu]);
            putc_("0123456789abcdef"[(dlh >>  8) & 0xfu]);
            putc_("0123456789abcdef"[(dlh >>  4) & 0xfu]);
            putc_("0123456789abcdef"[(dlh      ) & 0xfu]);
            puts_(" LCR=");
            putc_("0123456789abcdef"[(lcr_saved >> 12) & 0xfu]);
            putc_("0123456789abcdef"[(lcr_saved >>  8) & 0xfu]);
            putc_("0123456789abcdef"[(lcr_saved >>  4) & 0xfu]);
            putc_("0123456789abcdef"[(lcr_saved      ) & 0xfu]);
            putc_('\r'); putc_('\n');
            break;
        }

        case 'C': {
            /* RTC read via the compiled rtc_hd2_getTime() path -- end-to-end
             * verification of the IC_START (0xa0) fix.  Reply:
             * "RTC=HH:MM:SS DD/MM/YY\r\n" (each field 2 hex, binary values). */
            datetime_t t = rtc_hd2_getTime();
            puts_("RTC=");
            put_hex8((uint8_t)t.hour);   putc_(':');
            put_hex8((uint8_t)t.minute); putc_(':');
            put_hex8((uint8_t)t.second); putc_(' ');
            put_hex8((uint8_t)t.date);   putc_('/');
            put_hex8((uint8_t)t.month);  putc_('/');
            put_hex8((uint8_t)t.year);
            putc_('\r'); putc_('\n');
            break;
        }

        case 'L': {
            /* LED test: each colour ON 600ms with a 250ms all-off gap so GREEN
             * and YELLOW are unmistakable, then leave RED on.  Reply emitted
             * FIRST so the bridge captures it before the ~2.5s sequence. */
            puts_("LED cycle: GREEN, RED, YELLOW -> RED\r\n");
            platform_ledOff(GREEN); platform_ledOff(RED);  sleepFor(0u, 250u);
            platform_ledOn(GREEN);  sleepFor(0u, 600u); platform_ledOff(GREEN);  sleepFor(0u, 250u);
            platform_ledOn(RED);    sleepFor(0u, 600u); platform_ledOff(RED);    sleepFor(0u, 250u);
            platform_ledOn(YELLOW); sleepFor(0u, 600u); platform_ledOff(YELLOW); sleepFor(0u, 250u);
            platform_ledOn(RED);
            break;
        }

        case 'e': {
            /* Beep test: ascending 3-note tone via platform_beepStart/Stop
             * (PWM ch1 -> speaker; now unmutes the GPIOB.4 amp).  Reply first. */
            puts_("beep: 800/1200/1600 Hz\r\n");
            static const uint16_t notes[3] = { 800u, 1200u, 1600u };
            for (unsigned i = 0; i < 3u; ++i) {
                platform_beepStart(notes[i]);
                sleepFor(0u, 250u);
                platform_beepStop();
                sleepFor(0u, 80u);
            }
            break;
        }

        case 'g': {
            /* GPS test: init UART2 (PTA11/12 mux + 9600 8N1 via gps_HD2_init)
             * and stream raw RX bytes for ~2.5 s.  Expect NMEA "$GP../$GN.."
             * if the module is powered + streaming (visible even without a fix,
             * indoors).  Bytes land in the bridge log -- read 'rtx.py log'
             * after.  Reply marker emitted first. */
            puts_("GPS raw 2.5s:\r\n");
            const struct gpsDevice *gps = gps_HD2_init();
            gps->enable(gps->priv);         /* gated setup: UART2 + mux + UBX cfg */
            uint32_t gps_end = getTick() + 2500u;
            while ((int32_t)(getTick() - gps_end) < 0) {
                if ((UART2_LSR & LSR_RX_RDY) != 0u)
                    putc_((char)(UART2_RBR & 0xffu));
            }
            puts_("\r\n[gps end]\r\n");
            break;
        }

        case 'v': {
            /* Volume knob read: platform_getVolumeLevel() (ADC ch0 scaled). */
            puts_("VOL=0x"); put_hex8(platform_getVolumeLevel());
            putc_('\r'); putc_('\n');
            break;
        }

        case 'h': {
            /* Channel knob: poll platform_getChSelector() (GPIOB.5/6 quadrature)
             * for ~4s, emit the position whenever it changes -- turn the knob.
             * Bytes land in the bridge log (read 'log' after). */
            puts_("CHSEL poll 4s (turn knob):\r\n");
            int8_t last = (int8_t)0x7f;
            uint32_t end = getTick() + 4000u;
            while ((int32_t)(getTick() - end) < 0) {
                int8_t p = platform_getChSelector();
                if (p != last) {
                    puts_("ch=0x"); put_hex8((uint8_t)p); putc_('\r'); putc_('\n');
                    last = p;
                }
            }
            puts_("[chsel end]\r\n");
            break;
        }

        case 'J': {
            /* W25Q JEDEC-ID read -> six lowercase hex chars + CRLF.
             * Uses spi_hd2_read_jedec() (issues 0xAB wakeup first). */
            uint8_t id[3] = { 0 };
            spi_hd2_read_jedec(id);
            put_hex8(id[0]); put_hex8(id[1]); put_hex8(id[2]);
            putc_('\r'); putc_('\n');
            break;
        }

        case 'j': {
            /* DIAGNOSTIC per-bit trace of a JEDEC read: 48 LE u32 = 192 B. */
            uint32_t lo[32], hi[32];
            spi_hd2_jedec_trace(lo, hi);
            #define EMIT_U32(x) do { \
                uint32_t _v = (x); \
                putc_((char)(_v & 0xffu)); \
                putc_((char)((_v >>  8) & 0xffu)); \
                putc_((char)((_v >> 16) & 0xffu)); \
                putc_((char)((_v >> 24) & 0xffu)); \
            } while(0)
            for (int i = 0; i < 32; ++i) EMIT_U32(lo[i]);
            for (int i = 0; i < 32; ++i) EMIT_U32(hi[i]);
            #undef EMIT_U32
            break;
        }

        case 'V':
            /* Report firmware version + build id. */
            puts_("v" HD2_BUILD_VERSION " " HD2_BUILD_ID "\r\n");
            break;

        case 'b': {
            /* b <freq u16 LE>  -- play a tone via platform_beepStart() for
             * ~300 ms, then platform_beepStop().  freq=0 uses a 1 kHz
             * default.  Exercises the real OpenRTX speaker-tone path (PWM
             * ch1 + DIPLEX0 unmute), so the operator can audibly confirm
             * the beep API independent of the boot beep. */
            uint16_t f = (uint16_t)uart_getb();
            f |= (uint16_t)((uint16_t)uart_getb() << 8);
            if (f == 0u) f = 1000u;
            platform_beepStart(f);
            sleepFor(0u, 300u);
            platform_beepStop();
            putc_('k');
            break;
        }

        case 'l': {
            /* LCD test path A: bit-bang ST7735S init + RED fill. */
            extern void display_init(void);
            extern void display_render(void *fb);
            extern uint16_t *hd2_get_framebuffer(void);
            SOCSYS_IO_DIPLEX2 = 0x3ffffffbu;   /* PTC -> GPIO */
            puts_("LCD bit-bang: display_init...\r\n");
            display_init();
            uint16_t *fb = hd2_get_framebuffer();
            for (int i = 0; i < 160 * 128; ++i) fb[i] = 0xf800u;  /* red */
            display_render(fb);
            puts_("LCD bit-bang done -- screen should be RED\r\n");
            break;
        }

        case '8': {
            /* LCD test path B: route LCD pins to i8080 + dump 0x12000000. */
            SOCSYS_IO_DIPLEX2 = SOCSYS_IO_DIPLEX2 & ~0x0007ff00u;   /* DB0-7 + RD selects -> LCD func */
            puts_("DIPLEX2 LCD fields -> i8080 function\r\n");
            for (uint32_t o = 0; o <= 0x3c; o += 4) {
                puts_("  i8080+"); put_hex8((uint8_t)o); puts_("=");
                put_hex32(I8080_LCD_REG(o));
                puts_("\r\n");
            }
            break;
        }

        case 't': {
            /* Timebase check: tick, delayMs(1000), tick + delta (~0x3e8). */
            long long t0 = getTick();
            puts_("tick0="); put_hex32((uint32_t)t0); puts_("\r\n");
            delayMs(1000u);
            long long t1 = getTick();
            puts_("tick1="); put_hex32((uint32_t)t1);
            puts_(" delta="); put_hex32((uint32_t)(t1 - t0));
            puts_(" (expect ~0x3e8=1000)\r\n");
            break;
        }

        case 'i': {
            /* Periodic-IRQ test: VBR+PIC+Timer2@100Hz; ISR counter over 1s. */
            extern void irq_test_init(void);
            extern uint32_t irq_test_count(void);
            irq_test_init();
            uint32_t c0 = irq_test_count();
            puts_("irq0="); put_hex32(c0); puts_("\r\n");
            delayMs(1000u);
            uint32_t c1 = irq_test_count();
            puts_("irq1="); put_hex32(c1);
            puts_(" delta="); put_hex32(c1 - c0);
            puts_(" (expect ~0x64=100 at 100Hz)\r\n");
            break;
        }

        case 'F': {
            /* Force DFU/IAP entry WITHOUT the PTT+SK1 key combo.  Stage-1's
             * boot decision only reads two GPIOs (no flag we can set), so we
             * jump straight to the IAP crt0 entry @ 0x03201000 -- a complete
             * self-contained image that sets its own SP/VBR/UART (RE'd from
             * V2.1.3, see memory hd2-force-dfu).  Interrupts off first.
             * CAVEAT: the IAP crt0 inherits our PLL state (UART clock halved
             * to 42 MHz); if it comes up at the wrong baud we add a clock
             * fixup here.  Test outcome decides. */
            puts_("\r\n[force DFU -> IAP 0x03201000]\r\n");
            for (volatile int i = 0; i < 100000; ++i) { }
            /* Restore the IAP-handoff (~84 MHz) clock so the IAP's UART
             * comes up at 57600, not 28800 (our PLL halved it).  After
             * this our own UART desyncs -- fine, we jump away immediately. */
            { extern void clk_restore_prepll(void); clk_restore_prepll(); }
            __asm__ volatile (
                "psrclr ie\n\t"
                "lrw  r0, 0x03201000\n\t"
                "jmp  r0\n\t"
                : : : "r0"
            );
            while (1) { }
            break;
        }

        case 'a': {
            /* AT1846S RX-control-path self-test.  Runs the full chip init +
             * VCO calibration, programs FM/12.5kHz/145.5MHz, enables RX, and
             * reads back four registers.  Exercises the GPIOA bit-bang I2C
             * (independent of the dead HW-I2C2/ADC blocker).  init() spends
             * ~700 ms in calibration delays, so the host must allow time. */
            extern void hd2_radio_selftest(uint16_t *);
            uint16_t out[4] = { 0 };
            hd2_radio_selftest(out);
            puts_("AT1846S reg33=");
            put_hex8((uint8_t)(out[0] >> 8)); put_hex8((uint8_t)(out[0] & 0xffu));
            puts_(" rssi=");
            put_hex8((uint8_t)(out[1] >> 8)); put_hex8((uint8_t)(out[1] & 0xffu));
            puts_(" reg30=");
            put_hex8((uint8_t)(out[2] >> 8)); put_hex8((uint8_t)(out[2] & 0xffu));
            puts_(" rssi2=");
            put_hex8((uint8_t)(out[3] >> 8)); put_hex8((uint8_t)(out[3] & 0xffu));
            putc_('\r'); putc_('\n');
            break;
        }

        case 'q': {
            /* q <reg u8>  -- read an AT1846S register -> reply u16 LE.
             * Run 'a' once first to init the chip + bit-bang bus.  Use to
             * verify the audio bank (reg 0x15: 0x1100=FM / 0x1f00=DMR) and
             * probe the FM AF-output path live. */
            extern uint16_t hd2_at1846s_read(uint8_t);
            uint8_t reg = uart_getb();
            uint16_t v = hd2_at1846s_read(reg);
            putc_((char)(v & 0xffu));
            putc_((char)((v >> 8) & 0xffu));
            break;
        }

        case 'Q': {
            /* Q <reg u8> <val u16 LE>  -- write an AT1846S register; ack 'k'. */
            extern void hd2_at1846s_write(uint8_t, uint16_t);
            uint8_t reg = uart_getb();
            uint16_t v = (uint16_t)uart_getb();
            v |= (uint16_t)((uint16_t)uart_getb() << 8);
            hd2_at1846s_write(reg, v);
            putc_('k');
            break;
        }

        case 'w': {
            /* w <addr u8> <n u8> <bytes..>  -- generic bit-bang I2C write of n
             * bytes to the given 8-bit write address (e.g. 0x20 = FM tuner) on
             * the shared GPIOA bus.  Reply 'k'.  Use to drive arbitrary FM-tuner
             * registers / replay the init burst by hand during bring-up. */
            uint8_t addr = uart_getb();
            uint8_t n    = uart_getb();
            uint8_t buf[80];
            if (n > sizeof buf) n = sizeof buf;
            for (uint8_t i = 0; i < n; ++i) buf[i] = uart_getb();
            i2c0_write(addr, buf, n, true);
            putc_('k');
            break;
        }

        case 'p': {
            /* p <addr u8>  -- probe an 8-bit I2C write address; reply u8:
             * 0x00 = slave ACKed (present), 0xff = NACK.  Confirms the FM tuner
             * is on the bus at 0x20 (acceptance criterion: capture the ACK). */
            uint8_t addr = uart_getb();
            putc_((char)(i2c0_probe(addr) ? 0x00 : 0xff));
            break;
        }

        case 'f': {
            /* f <freq_kHz u32 LE>  -- FM-broadcast one-shot: powerup + tune +
             * route to speaker.  freq 0 -> 97700 kHz (97.7 MHz).  Reply 'k'. */
            uint32_t khz = (uint32_t)uart_getb();
            khz |= (uint32_t)uart_getb() <<  8;
            khz |= (uint32_t)uart_getb() << 16;
            khz |= (uint32_t)uart_getb() << 24;
            if (khz == 0u) khz = 97700u;
            fm_broadcast_powerup();
            fm_broadcast_tune(khz);
            fm_broadcast_route_speaker();
            putc_('k');
            break;
        }

        case 'N': {
            /* N <src u8> <sink u8>  -- connect an audio path via the audio_HD2
             * matrix.  Reply u8: 0x00 ok, 0xff refused (incompatible / unarmed
             * TX / bad id).  src: 0=MIC 1=RTX 2=MCU; sink: 0=SPK 1=RTX 2=MCU. */
            uint8_t src = uart_getb();
            uint8_t snk = uart_getb();
            putc_((char)(hd2_router_connect(src, snk) == 0 ? 0x00 : 0xff));
            break;
        }

        case 'n': {
            /* n <src u8> <sink u8>  -- disconnect an audio path.  Reply u8. */
            uint8_t src = uart_getb();
            uint8_t snk = uart_getb();
            putc_((char)(hd2_router_disconnect(src, snk) == 0 ? 0x00 : 0xff));
            break;
        }

        case 'S': {
            /* S <target u8> <val u32 LE>  -- set a routing target.  Reply u32 LE
             * readback, or 0xffffffff if the target is guarded and TX is not
             * armed (send 'E' first).  See hd2_router.h for the target enum. */
            uint8_t  t = uart_getb();
            uint32_t v = (uint32_t)uart_getb();
            v |= (uint32_t)uart_getb() <<  8;
            v |= (uint32_t)uart_getb() << 16;
            v |= (uint32_t)uart_getb() << 24;
            uint32_t r = hd2_route_set(t, v);
            putc_((char)(r & 0xffu));
            putc_((char)((r >>  8) & 0xffu));
            putc_((char)((r >> 16) & 0xffu));
            putc_((char)((r >> 24) & 0xffu));
            break;
        }

        case 'E':
            /* Arm the guarded routing ops (PA enable / TX mode / power-hold)
             * for the NEXT guarded 'S' or TX 'N'.  One-shot. */
            hd2_route_arm_tx();
            puts_("TX armed\r\n");
            break;

        case 'U': {
            /* Routing snapshot: decode DIPLEX / voice_path / af_gate / codec /
             * amp / AT1846S reg30 to the console. */
            char buf[320];
            hd2_route_dump(buf, sizeof buf);
            puts_(buf);
            break;
        }

        case 'A': {
            /* Start the OpenRTX UI superloop (cooperative; never returns).
             * The loader stays live -- hd2_app_run() calls loader_cmd_poll()
             * each iteration, so R/W/keys/probe keep working while the UI
             * runs.  Power-cycle to get back to the bare loader. */
            extern void hd2_app_run(void);
            puts_("Starting OpenRTX app superloop (loader stays live)...\r\n");
            hd2_app_run();
            break;
        }

        case '?':
            puts_(HELP);
            break;

        default:
            /* drop quietly so stray bytes from screen-attach noise
             * don't trigger anything. */
            break;
    }
}

int loader_cmd_poll(void)
{
    /* Reading RBR consumes from the FIFO, so check LSR.DR then a single
     * RBR read -- never peek-then-get (that pops twice). */
    if ((UART0_LSR & LSR_RX_RDY) == 0)
        return 0;
    activity_kick();             /* any input reactivates backlight + zeroes timer */
    uint8_t c = (uint8_t)(UART0_RBR & 0xffu);
    loader_cmd_dispatch(c);
    return 1;
}

int main(void)
{
    platform_init();
    boot_beep();
    red_on();

    puts_("\r\nOpenRTX HD2 loader v" HD2_BUILD_VERSION " (build " HD2_BUILD_ID ")\r\n");
    puts_("post-init, RED on\r\n");
    puts_("Starting backlight (level 76, default 30 s inactivity timeout)\r\n");
    backlight_init();
    backlight_enable();

    puts_("Init keyboard driver (matrix on GPIOC, side keys on GPIOB)\r\n");
    kbd_init();

    /* Bring the W25Q SPI-NOR flash up.  spi_hd2_init() must run before
     * W25Qx_init() because the latter immediately drives CS via the
     * gpioPin vtable -- which assumes the bus pins are already in a
     * known direction.  Failures here are silent (no return value to
     * check) but a follow-up 'J' command will surface them. */
    spi_hd2_init();
    /* W25Qx_init() temporarily skipped — its wakeup command at MCU speed
     * seems to leave the chip unresponsive to subsequent JEDEC probes
     * (firmware J returns 0x000000 even though host-side bitbang at the
     * same speeds returns 0xef4020).  Without wakeup we may be operating
     * on a chip that's in deep power-down, but the host-side test on the
     * running radio suggests it stays awake out of reset. */
    /* W25Qx_init(&eflash); */
    puts_("SPI flash bus up (spi_hd2_init only) -- send 'J' to probe JEDEC-ID\r\n");

    /* NOTE: display_init() intentionally NOT called -- we want to leave
     * the LCD bus in its IAP-default state so the loader can drive it
     * cleanly to hunt for the chip's power-enable GPIO.  Backlight LED
     * still lights because that's a separate PWM ch0 path.            */
    puts_("display_init SKIPPED (loader-driven init mode)\r\n");

    loader_cmd_banner();

    /* Bare loader loop: poll the command module; when idle, blink the
     * RED LED + run the backlight inactivity countdown.  The same
     * loader_cmd_poll() is called from the OpenRTX app superloop, so
     * probe/MMIO access stays live there too. */
    unsigned cycle = 0;
    while (1) {
        if (loader_cmd_poll())
            continue;

        keyecho_tick();             /* stream key names while 'y'-mode is on */

        if ((cycle & 0xfffff) == 0)         red_off();
        if ((cycle & 0xfffff) == 0x80000)   red_on();
        ++cycle;

        /* ~10 ms idle tick so inactivity_ms counts wall-clock time. */
        sleepFor(0u, 10u);
        inactivity_ms += 10u;
        if (backlight_timeout_ms != 0u && inactivity_ms >= backlight_timeout_ms) {
            backlight_disable();
        }
    }
    return 0;
}
