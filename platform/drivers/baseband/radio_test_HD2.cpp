/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Live AT1846S RX tune + RSSI sweep harness for the HD2 OpenRTX loader.
 *
 * Driven by the loader 'a' command (hd2_radio_selftest).  Designed so the
 * host can SWEEP frequencies WITHOUT reflashing: the target frequency is a
 * plain RAM global (g_fm_test_freq) the host pokes via the loader 'W'
 * command (write its .map address) before each 'a' call.  Sweep loop:
 *     for f in band:  W g_fm_test_freq f ;  a ;  parse rssi
 *
 * Each 'a' call:
 *   1. (first call only) AT1846S::init() + FM/25 kHz + enable the RX front
 *      end, then mark inited -- so subsequent sweep points are fast retunes.
 *   2. setFrequency(g_fm_test_freq) + re-assert RX.
 *   3. settle, then sample RSSI (reg 0x1B upper byte) x2 + mode reg 0x30.
 *
 * RX FRONT END: the vendor's at1846s_rx_path_open (0x030405a0) sets AT1846S
 * reg 0x30 bit5 (RX_ON) AND gpio_out_set(0x24) == GPIOB.4 -- the board-level
 * RX enable (antenna/LNA switch to the receiver).  We replicate both here so
 * the antenna actually reaches the chip; otherwise RSSI only sees the noise
 * floor.  This is independent of the dead HW-I2C2/ADC blocker: the AT1846S
 * is on the GPIOA bit-bang bus (SCL=PTA7/SDA=PTA8, slave 0xE2).
 */

#include "drivers/baseband/AT1846S.h"
#include "drivers/i2c_csky.h"
#include "hd2_regs.h"
#include <stdint.h>

/* AT1846S I2C slave address (== 0x71 << 1), matches AT1846S_HD2.cpp. */
static constexpr uint8_t AT1846S_ADDR = 0xE2;

/*
 * GPIOB (DW_apb_gpio, base 0x14100000) FM RX audio gates -- GPIOB_DR/DDR +
 * SPKR_AMP_BIT (GPIOB.4) + AUDIO_ROUTE_BIT (GPIOB.10) come from hd2_regs.h
 * (from vendor spkr_amp_unmute 0x03040fa4 + audio_route_rx_unmute 0x03041ec0):
 *   GPIOB.4  (pin 0x24) = speaker-amp MUTE, active-LOW: LOW = playing.
 *   GPIOB.10 (pin 0x2a) = RX audio route (default path), clear = routed.
 */
#define SPKR_MUTE_BIT  SPKR_AMP_BIT

/*
 * Audio on/off, host-pokeable via 'W'.  1 (default) = run the FM-RX audio
 * enable (unmute amp + route + AT1846S reg40); 0 = leave speaker muted
 * (RSSI-only sweep, as before).
 */
extern "C" { volatile uint32_t g_fm_audio_on = 1u; }

/* RX audio volume (AT1846S reg 0x44 low byte; reg = 0x900 | vol).  Vendor
 * sources this from g_tune_data[2]; our loader has no volume setting, so it
 * defaults near-max and is host-pokeable via 'W' for a live sweep. */
extern "C" { volatile uint32_t g_fm_volume = 0x3fu; }

/* Diagnostic: OR of every modem RX IRQ-latch (0x11000398) value seen during the
 * servicing loop. Host reads it after 'a' to tell if the modem RX FSM is alive
 * (value changes from idle 0x4000) or clock-dead. */
extern "C" { volatile uint32_t g_modem_irq_acc = 0u; }

/*
 * RF FREEZE (loader op 'z').  While 1, ALL firmware-initiated AT1846S I2C
 * traffic and audio-GPIO/diplex rewrites are suspended so a host can run live
 * chip experiments over 'q'/'Q'/'S' without the firmware overwriting them
 * within seconds.  Gated call sites (threads keep running, they just skip
 * chip I/O):
 *   - hd2_rtx.c rtx_task()            (33 Hz RSSI poll + squelch audio gate
 *                                      + reconfigure -> radio_* AT1846S writes)
 *   - radio_HD2.cpp radio_enableRx / radio_disableRtx / radio_setOpmode /
 *     radio_updateConfiguration / radio_enableAfOutput / radio_disableAfOutput
 *     / radio_getRssi (returns last cached value while frozen)
 *   - audio_HD2.c audio_connect / audio_disconnect  (PTB4/PTB10 amp+route)
 *   - platform.c platform_beepStart / platform_beepStop  (PTB4/PTB10 +
 *     DIPLEX0 bit18 PWM-audio mute; beepStop still stops the PWM channel)
 *   - hd2_fm_probe.cpp fmThread       (broadcast-tuner I2C on the SAME
 *                                      GPIOA bit-bang bus + PTB10/PTB20)
 * Host-initiated diag ops (q/Q/S/I/o/m/u) are intentionally NOT gated.
 */
extern "C" { volatile uint32_t g_rf_freeze = 0u; }

/*
 * HOST-TUNABLE sweep frequency, in Hz.  Poke via the loader 'W' command at
 * this global's .map address, then issue 'a' to tune + read RSSI.  Default
 * 103.600 MHz (a strong broadcast station found this session) so the audio
 * test lands on a real signal.  For broadcast FM, sweep ~87.5e6 .. 108e6.
 */
extern "C" { volatile uint32_t g_fm_test_freq = 103600000u; }

/* Re-init only once; sweep points after the first are fast retunes. */
static bool g_radio_test_inited = false;

/* Coarse settle delay (~ms) so the RSSI detector tracks the new tune. */
static void rssi_settle(void)
{
    for (volatile uint32_t i = 0; i < 200000u; ++i) { }
}

/*
 * Read one 16-bit AT1846S register over the GPIOA bit-bang bus, using the
 * exact same primitives + big-endian byte order as AT1846S::i2c_readReg16.
 */
static uint16_t at1846s_read_reg(uint8_t reg)
{
    uint16_t value = 0;

    i2c0_lockDeviceBlocking();
    i2c0_write(AT1846S_ADDR, &reg, 1, false);
    i2c0_read(AT1846S_ADDR, &value, 2);
    i2c0_releaseDevice();

    /* AT1846S sends register data big-endian on the wire. */
    return __builtin_bswap16(value);
}

/* Write one 16-bit AT1846S register (reg, then value big-endian), same wire
 * shape as AT1846S::i2c_writeReg16. */
static void at1846s_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xff) };
    i2c0_lockDeviceBlocking();
    i2c0_write(AT1846S_ADDR, buf, 3, true);
    i2c0_releaseDevice();
}

/*
 * HR_C7000 PCM/audio-codec block (byte-addressed MMIO @ 0x16000900, verified
 * alive + readable on hardware).  Port of boot_init_modem_audio_path
 * (0x0305e62c) + the analog-FM tail of boot_init_audio_route (0x0305e7d8).
 * MUST use byte writes -- the block ignores partial word writes.
 */
/* CODEC_BASE + CODEC_BYTE(off) come from hd2_regs.h (byte-addressed PCM
 * block).  Keep the short local CB() name this file already uses. */
#define CB(off)      CODEC_BYTE(off)
#define G_TUNE_DATA  (*(volatile uint8_t  *)0x00043974u)

/* Codec DAC output gain (GCR_DACL @ codec 0xdf): vendor sources the low 6 bits
 * (godl) from codeplug calibration g_tune_data[2] | 0x80.  Our loader has no
 * codeplug, so G_TUNE_DATA reads garbage (0x55 -> 0xd5) -- a WRONG DAC gain
 * that attenuates the codec output (live-diffed 2026-06-02: vendor playing FM
 * had 0xdf=0xb4=0x34|0x80, ours 0xd5).  Use the vendor's known-good value so
 * the codec DAC isn't mis-gained.  See [[project_hd2_fm_rx_tune_verified]]. */
#define CODEC_DACL_GAIN  0xb4u   /* = g_tune_data 0x34 | 0x80 (vendor FM) */

/* HR_C7000 socsys audio/codec control registers (base 0x11000000) all come
 * from hd2_regs.h (SOCSYS_SYS_SOFT_RSTN, SOCSYS_DAC_CONTROL, SOCSYS_ADC_CONTROL,
 * SOCSYS_VOICE_PATH, SOCSYS_PCM_MODE, SOCSYS_LINEOUT_CTRL, SOCSYS_WORK_MODE,
 * SOCSYS_AF_GATE, SOCSYS_MODEM_RXDP0/1, SOCSYS_MODEM_IRQ/_ACK).  Offsets
 * verified live against the vendor while it played FM 107.5 -- see
 * tmp/vendor_fm_playing_state.md. */
#define SYS_SOFT_RSTN       SOCSYS_SYS_SOFT_RSTN
/* 0x088 standby_lo (bit31) doubles as the codec-DAC-enable handshake flag used
 * during bring-up; the codec init below waits on it under this same name. */
#define PCM_HANDSHAKE       SOCSYS_LINEOUT_CTRL

static void delay_ms(unsigned ms)
{
    for (unsigned m = 0; m < ms; ++m)
        for (volatile unsigned i = 0; i < 8000u; ++i) { }
}

static bool g_codec_inited = false;

static void hd2_codec_audio_init(void)
{
    /* PCM block reset pulse + codec soft-reset (matches vendor exactly). */
    CB(0xd2) |= 0x03;
    SYS_SOFT_RSTN &= 0xffffffefu;      /* clear bit4 = codec reset */
    delay_ms(2);
    CB(0xd2) &= ~0x01;
    delay_ms(100);
    CB(0xd2) &= ~0x02;
    CB(0xd3) = 0x40;
    CB(0xcb) = 0x00;
    CB(0xcc) |= 0x40;
    CB(0xc9) = 0xc0;
    CB(0xcf) &= ~0x10;
    delay_ms(100);
    CB(0xcf) &= ~0x80;
    CB(0xc8) = 0xc0;
    CB(0xcd) &= ~0x10;

    /* Wait for the PCM handshake (socsys 0x88 bit31 clear), bounded. */
    for (uint32_t g = 0; g < 200u; ++g) {
        if ((PCM_HANDSHAKE & 0x80000000u) == 0u) break;
        delay_ms(10);
    }

    CB(0xcd) &= ~0x80;
    delay_ms(100);
    CB(0xdf) = CODEC_DACL_GAIN;
    CB(0xe5) = 0x8b;
    PCM_HANDSHAKE = 1;

    /* boot_init_audio_route analog tail (same block). */
    CB(0xc8) = 0xc0;
    CB(0xcd) = 0x20;
    CB(0xe5) = 0x8b;
    CB(0xdf) = CODEC_DACL_GAIN;
    SOCSYS_PCM_MODE |= 0x02;           /* _hrc7000_pcm_mode |= 2 */
}

/*
 * Warm up the HR_C7000 codec audio-OUTPUT path ONCE: codec bring-up + the socsys
 * audio gate, with NO AT1846S RF tune and NO modem-RX servicing loop.  On the
 * OpenRTX bench unit the PWM-ch1 beep mixes THROUGH the codec (DAC -> Mercury PWM
 * lineout -> speaker), so the codec must be initialised before a beep is audible
 * -- proven 2026-06-01: beeps were silent until 'a' ran, then rang clearly.  This
 * is the minimal subset of 'a' needed for that, minus the 3 s servicing loop that
 * wedges the loader.  Called lazily from platform_beepStart.  GPIOB.4/.10 (amp +
 * route) are left to platform_beepStart so they re-assert per beep. */
extern "C" void hd2_audio_out_warm(void)
{
    static bool warmed = false;
    if (warmed) return;
    if (!g_codec_inited) { hd2_codec_audio_init(); g_codec_inited = true; }
    SOCSYS_DAC_CONTROL  = 0x8000001fu;
    SOCSYS_ADC_CONTROL  = 0x000041c3u;
    SOCSYS_VOICE_PATH   = 0x00000002u;
    SOCSYS_PCM_MODE     = 0x00000000u;
    SOCSYS_LINEOUT_CTRL = 0x00000003u;   /* both lineouts; speaker is on LINE2OUT (see FM path) */
    SOCSYS_WORK_MODE    = 0x0000006eu;
    SOCSYS_AF_GATE      = 0x0001007fu;
    SOCSYS_MODEM_RXDP0  = 0x000000c4u;
    SOCSYS_MODEM_RXDP1  = 0x00000040u;
    warmed = true;
}

/*
 * AT1846S RX-audio (AF-DSP) config -- the chip-side half of the proven
 * 'a'-command listen sequence, WITHOUT the codec/socsys gate and WITHOUT any
 * board GPIO.  Call ONCE per RX entry (radio_enableRx); it's a ~12-write I2C
 * burst + a setBandwidth filter-bank load, far too heavy to run on every
 * squelch crossing (that wedged the bit-bang bus -- task: squelch lockup).
 *
 * Sets reg 0x40=0x30 (AF-DSP LISTEN, not 0x11 scan-mute), apply_rx_audio_config
 * (filter/AGC/output + VOLUME via g_fm_volume), then reg 0x7a + the FM filter
 * bank (else the DMR bank runs on FM -> no demod audio).  Uses the file-static
 * at1846s_write_reg helper (the proven bit-bang path).  No codec: the analog
 * AF reaches the speaker over the same route the broadcast tuner uses.
 *
 * KEEP IN SYNC with the AT1846S writes in hd2_radio_selftest's g_fm_audio_on
 * block below (the live-verified reference).
 */
extern "C" void hd2_at1846s_rx_audio_config(void)
{
    at1846s_write_reg(0x40, 0x0030);         /* RX AF-DSP LISTEN (NOT 0x11 scan-mute) */

    at1846s_write_reg(0x41, 0x471e);
    at1846s_write_reg(0x44, 0x0900u | (uint16_t)(g_fm_volume & 0xff));
    at1846s_write_reg(0x33, 0x44a5);
    at1846s_write_reg(0x54, 0x2a3c);
    at1846s_write_reg(0x63, 0x16ad);
    at1846s_write_reg(0x58, 0x8405);
    at1846s_write_reg(0x4e, 0x6002);

    at1846s_write_reg(0x7a, 0xa00a);
    AT1846S::instance().setBandwidth(AT1846S_BW::_25);   /* FM bank 0 (vendor-confirmed) */

    /* 0x3a (voice-channel/audio-path select) MUST come AFTER setBandwidth: the
     * FM bank table includes 0x3a=0x00c3 and silently clobbers the vendor FM-RX
     * value (live readback 2026-06-10: our radio held 0x00c3, vendor uses 0x80e1).
     * NOTE: re-poking 0x80e1 live did NOT restore audio by itself -- kept for
     * vendor-state parity, not as the audio fix. */
    at1846s_write_reg(0x3a, 0x80e1);

    /* Vendor FM-RX reg-0x30 enable (RE: rf_apply_channel_to_pll FM branch does
     * 0x4806 then 0x4826).  setFuncMode left it at 0x4026 -- missing bit11 (0x800),
     * the RX-side path bit.  Persist it here (a live poke got overwritten on RX
     * re-entry).  0x4826 = band(0x4000)+RX-side(0x800)+RX_ON(0x20)+0x06. */
    at1846s_write_reg(0x30, 0x4806);
    at1846s_write_reg(0x30, 0x4826);
    /* NOTE: reverted the SA8x8 voice-channel (0x3A|=0x4000) + 0x57 pokes -- the vendor
     * HD2 FM path never does them (RE 2026-06-09). */
}

/*
 * Complete analog-FM RX audio bring-up on the HR_C7000 side: codec DAC power-up
 * (once) + the modem analog-FM audio gate (socsys).  This is what lets the
 * AT1846S-IF FM demod reach the codec DAC -> lineout -> speaker.  Call once per
 * RX entry (radio_enableRx), AFTER hd2_at1846s_rx_audio_config().
 *
 * EVERY value here was verified LIVE against a vendor radio playing FM out the
 * speaker, by word-diffing its socsys/codec block over dbgshell (2026-06-09):
 *   work_mode 0x6e, af_gate 0x1007f, rf_mode(0x104) 0x034c9060, codec DAC out of
 *   standby with gain 0xb4.  Our threaded build otherwise leaves the codec in
 *   sleep/standby (CR_VIC=0x0f, CR_DAC=0xb0, GCR_DACL=0x00) -> only amp noise,
 *   no demod audio; and it NEVER programmed rf_mode (the baseband RF-interface
 *   mode) so the modem had no path to route the demod onto the DAC.
 *
 * Codec init is latched (g_codec_inited) -> runs once.  The socsys gate is set
 * unconditionally every RX entry (it must NOT depend on the hd2_audio_out_warm
 * 'warmed' latch, which a boot beep can consume first, leaving the FM gate
 * unset).  Codec config goes through the file-static hd2_codec_audio_init (the
 * proven MC-interface access; the codec block can't be poked from a cold host).
 */
extern "C" void hd2_modem_fm_boot_init(void)
{
    /* ONE-TIME modem RX + baseband-IF-ADC + codec bring-up.  Call ONCE from
     * radio_init (rtx thread startup), BEFORE any RX -- the modem isn't in use yet,
     * so resetting it can't hang a mid-flight AHB transaction, and the config is
     * written once (NOT per RX entry, which hung the bus, 2026-06-09).
     *
     * Reset value 0x1d0 (SYS_SOFT_RSTN, active-low, auto-releasing) holds
     * protocol[0]+phy[1]+fm[2]+audio[3]+adc[5] in reset, RELEASES codec[4]+
     * adc_ctrl[6]+sys[7]+cpu[8].  This is the vendor's boot value 0x190 MINUS bit6
     * (adc_ctrl): we must NOT reset the CPU/volume ADC at 0x140d0000 -- the UI/main
     * threads are already polling it for the volume knob + battery, and resetting it
     * mid-run hung the whole system.  bit5 (the baseband IF ADC) IS reset -- that's
     * the one the modem demod needs, and the one the earlier failed 0x1f8 test omitted. */
    /* FM-RX CLOCK GATE (the missing piece, 2026-06-10).  CLK_MGR @0x2c boot value
     * 0xfff0ff3c (BSP clk_init) leaves [7]modem_clk_fmrx_en + [6]modem_clk_fmtx_en
     * OFF -- so the analog-FM-RX baseband datapath has NO CLOCK and stays
     * configured-but-not-running (adc_testout=0; PHASE_OUT reads wedge the AHB --
     * a read into an unclocked block hangs the bus).  [9]modem_clk_rx_en is ON but
     * that's the DMR/baseband RX clock, NOT FM.  Manual §4.2.5.11 [7]=FM-RX clock
     * gate, active-high.  Blind write of boot-value|0xC0 (enable both FM clocks):
     * no read (modem-MMIO reads wedge our loader) and no clock gets disabled, so
     * safe from the rtx thread.  Must precede the reset + datapath config below. */
    SOCSYS_REG2C = 0xfff0fffcu;

    SOCSYS_SYS_SOFT_RSTN = 0x000001d0u;

    /* Codec power-up (the audio block was just reset). */
    g_codec_inited = false;
    hd2_codec_audio_init();
    g_codec_inited = true;

    /* AF-RECEIVE BIAS (the fix, RE 2026-06-09): in AF mode (RF_MODE[24]=1) the
     * AT1846S audio enters the baseband ADC's VINP, and the ADC's VINM/VCM must be
     * biased by the CPU-bus DAC (manual §8.2.3.1; ADC_CONTROL adc_extcm=1+semode=1).
     * Our build never programmed it, so the AF ADC had no operating point and never
     * sampled (adc_testout=0 -> no demod audio).  Vendor dac_controller_init @0x0305960C:
     * route the DAC analog-out pins (DIPLEX2 PTC-mux bits 28/29 clear), power up DAC
     * channels B+C, and drive channel C to the AF bias 0x6E2. */
    SOCSYS_IO_DIPLEX2 &= 0xCFFFFFFFu;    /* route DAC out pins (clear PTC mux bits 28-29) */
    DAC_MCU_PD_MODE_EN = 0x00000001u;
    DAC_MCU_DATA_A     = 0x00000000u;
    DAC_MCU_DATA_B     = 0x00000000u;
    DAC_MCU_DATA_C     = 0x00000000u;
    DAC_MCU_PD_CTRL    = 0x00000001u;    /* power-down A (bit0=1), power-up B+C */
    DAC_MCU_DATA_C     = 0x000006E2u;    /* AF-receive single-point bias (vendor value) */

    /* Modem analog-FM datapath gate + RF/IF/AGC -- written ONCE here, after the reset.
     * Values verified live against a vendor radio playing FM (dbgshell, 2026-06-09). */
    SOCSYS_DAC_CONTROL  = 0x8000001fu;
    SOCSYS_ADC_CONTROL  = 0x000041c3u;   /* enadc0[8]+enadc1[7]+adc_enref[6] (baseband IF ADC) */
    SOCSYS_VOICE_PATH   = 0x00000002u;   /* bit0=0 -> audio via codec DAC (FM) */
    SOCSYS_PCM_MODE     = 0x00000000u;
    SOCSYS_LINEOUT_CTRL = 0x00000001u;   /* line2out only -- vendor live value (was 0x03) */
    SOCSYS_WORK_MODE    = 0x0000006eu;   /* FM-analog (vendor live) */
    SOCSYS_RF_MODE      = 0x034c9060u;   /* RF_MODE: AF-receive (vendor live) */
    SOCSYS_REG(0x110u)  = 0x00041f1au;   /* RF_CONTROL (vendor live) */
    SOCSYS_REG(0x114u)  = 0x01e80000u;   /* RF/IF reg (vendor live; was 0 on ours) */
    SOCSYS_REG(0x120u)  = 0x0978786fu;   /* THRESHOLD_VALUE: arrival/timing-sync detect
                                          * (vendor live; we never set it -- §8.2.2 ties it
                                          * to ADC-receive; may gate the AF datapath). */
    SOCSYS_REG(0x168u)  = 0x00000014u;   /* slot_guard (vendor live) */
    SOCSYS_REG(0x1b0u)  = 0x000bb800u;   /* RX_IF_FREQ = 768 kHz */
    SOCSYS_REG(0x1b4u)  = 0x000036b0u;   /* RX_AGC (vendor live) */
    SOCSYS_AF_GATE      = 0x0001007fu;   /* FM-RX audio gate (vendor live) */
    SOCSYS_MODEM_RXDP0  = 0x000000c4u;   /* modem RX datapath shadow */
    SOCSYS_MODEM_RXDP1  = 0x00000040u;
}

extern "C" void hd2_radio_selftest(uint16_t out[4])
{
    AT1846S& at = AT1846S::instance();

    if (!g_radio_test_inited)
    {
        /* Full chip init (VCO calibration dance, ~700 ms of delays). */
        at.init();
        at.setOpMode(AT1846S_OpMode::FM);
        at.setBandwidth(AT1846S_BW::_25);    /* widest filter (closest to BCFM) */

        /* Make the two audio-gate GPIOs outputs; start with the speaker
         * amp MUTED (GPIOB.4 HIGH), like the vendor does while tuning. */
        GPIOB_DDR |= (SPKR_MUTE_BIT | SPKR_GAIN_BIT | AUDIO_ROUTE_BIT);
        GPIOB_DR  |= SPKR_MUTE_BIT;

        g_radio_test_inited = true;
    }

    /* 1. reg 0x33 readback proves the I2C R/W path + that init ran (0x45F5
     *    region after calibration; 0x0000/0xFFFF => dead bus). */
    out[0] = at1846s_read_reg(0x33);

    /* 2. Tune to the host-selected frequency and enable RX. */
    at.setFrequency(g_fm_test_freq);
    at.setFuncMode(AT1846S_FuncMode::RX);
    rssi_settle();

    /* 3. FM RX audio path (vendor radio_transition_tx_to_rx + audio_route_rx_unmute):
     *    AT1846S reg 0x40 = 0x30, route RX audio (GPIOB.10 LOW), unmute the
     *    speaker amp (GPIOB.4 LOW).  Gated by g_fm_audio_on so the host can
     *    A/B against a muted RSSI-only sweep. */
    if (g_fm_audio_on)
    {
        if (!g_codec_inited) { hd2_codec_audio_init(); g_codec_inited = true; }

        /* AT1846S RX AF-DSP enable. reg0x40=0x30 is the vendor LISTEN value
         * (radio_transition_tx_to_rx @0x030415e8); bit 0x20 = RX AF-DSP path on.
         * 0x11 (what we had) is the vendor SCAN-MUTE value (AF attenuated between
         * scan dwells, task_rf_retune @0x030403a4) -- copying it onto the listen
         * path gave faint hiss / no program audio. */
        at1846s_write_reg(0x40, 0x0030);     /* vendor FM-RX LISTEN (AF-DSP enable) */

        /* AT1846S RX audio output config -- vendor at1846s_apply_rx_audio_config
         * (0x03058f84), no-tone FM path: analog audio filter/AGC + output + VOLUME. */
        at1846s_write_reg(0x3a, 0x80e1);
        at1846s_write_reg(0x41, 0x471e);
        at1846s_write_reg(0x44, 0x0900u | (uint16_t)(g_fm_volume & 0xff));
        at1846s_write_reg(0x33, 0x44a5);
        at1846s_write_reg(0x54, 0x2a3c);
        at1846s_write_reg(0x63, 0x16ad);
        at1846s_write_reg(0x58, 0x8405);
        at1846s_write_reg(0x4e, 0x6002);

        /* Vendor analog-FM order (rf_apply_channel_to_pll @0x03059090): AFTER
         * apply_rx_audio_config, write reg0x7a then (re)load the FM audio-DSP
         * bank.  apply_rx_audio_config left reg0x3a=0x80e1; the FM bank restores
         * it to 0x00c3 (+ the 0x06..0x12 page-1 EQ coefficients).  WITHOUT this
         * the chip ran the DMR filter bank on an FM signal -> no demod audio. */
        at1846s_write_reg(0x7a, 0xa00a);
        at.setBandwidth(AT1846S_BW::_25);     /* now selects FM bank (index 0) */

        /* HR_C7000 analog-FM audio gate -- EXACT vendor values captured LIVE
         * via dbgshell word-reads from the vendor radio *while it played FM
         * 107.5* (2026-06-01).  Every socsys reg below now matches the
         * vendor's playing state; our prior guessed values (WORK_MODE=0x22,
         * af_gate=0x800, pcm_mode=3, voice_path bit0-clear, LINEOUT unset)
         * were all wrong.  KEY: the modem RX latch (0x11000398) reads 0 on the
         * vendor *while playing FM* -> FM broadcast is pure-analog through the
         * codec; the digital modem need not run (kills the modem-run theory).
         * No soft-reset hold needed -- vendor steady-state is 0x1ff. */
        SOCSYS_DAC_CONTROL  = 0x8000001fu;
        SOCSYS_ADC_CONTROL  = 0x000041c3u;
        SOCSYS_VOICE_PATH   = 0x00000002u;   /* bit1 set                  */
        SOCSYS_PCM_MODE     = 0x00000000u;
        SOCSYS_LINEOUT_CTRL = 0x00000003u;   /* line1out_en|line2out_en: HW-verified
                                              * 2026-06-02 the HD2 speaker is on LINE2OUT
                                              * (bit0). Writing 0x01 alone gets auto-flipped
                                              * to 0x02 (line1out) by the modem -> wrong pin
                                              * -> silence. Forcing both keeps line2out driven:
                                              * total silence -> audible codec output. */
        SOCSYS_WORK_MODE    = 0x0000006eu;   /* vendor FM-analog value    */
        SOCSYS_AF_GATE      = 0x0001007fu;   /* vendor FM-RX audio gate   */
        SOCSYS_MODEM_RXDP0  = 0x000000c4u;
        SOCSYS_MODEM_RXDP1  = 0x00000040u;

        GPIOB_DR &= ~AUDIO_ROUTE_BIT;        /* GPIOB.10 LOW -> route RX audio */
        GPIOB_DR &= ~SPKR_MUTE_BIT;          /* GPIOB.4  LOW -> speaker unmute  */
        GPIOB_DR |=  SPKR_GAIN_BIT;          /* GPIOB.17 HIGH -> amp full gain  */

        /* CONTINUOUS modem-RX servicing (the piece our superloop lacks): mimic
         * task_sysint -- drain the modem RX IRQ latch (0x11000398) -> ACK
         * (0x110003a0) every tick so the modem FSM keeps pumping the hardware
         * FM-audio datapath.  Run ~3 s so the user can listen during the call.
         * g_modem_irq_acc accumulates every latch value seen -> if it stays
         * 0x4000 (idle) the FSM never raised an RX IRQ = clock-dead (#90/#94);
         * if it changes, the modem RX FSM is alive. */
        /* Short settle drain only. FM broadcast is pure-analog through the codec
         * (modem latch reads 0 on the vendor while playing FM), so the continuous
         * modem-RX servicing is NOT needed for audio -- and a long busy-wait here
         * would freeze the threaded-build UI. The FM worker thread keeps audio
         * alive after this returns. (Was 300*10ms=3s; now ~0.2s.) */
        g_modem_irq_acc = 0;
        for (uint32_t k = 0; k < 20u; ++k)
        {
            uint32_t v = SOCSYS_MODEM_IRQ;
            g_modem_irq_acc |= v;
            SOCSYS_MODEM_IRQ_ACK = v;                  /* ACK */
            delay_ms(10);
        }
    }
    else
    {
        GPIOB_DR |= SPKR_MUTE_BIT;           /* keep speaker muted */
    }

    /* 4. RSSI (reg 0x1B upper byte = level) + mode/status (0x30) + 2nd RSSI. */
    out[1] = at1846s_read_reg(0x1B);
    out[2] = at1846s_read_reg(0x30);
    out[3] = at1846s_read_reg(0x1B);
}

/* Live AT1846S register access for the loader 'q'/'Q' commands.  Lets us
 * verify the loaded audio bank (read reg 0x15: 0x1100=FM / 0x1f00=DMR) and
 * binary-search the FM AF-output path while listening, without a reflash.
 * The chip must have been inited first (run 'a' once). */
/* Route through the file-static at1846s_read_reg/at1846s_write_reg helpers,
 * NOT the AT1846S class methods.  The class i2c_readReg16 -- though textually
 * identical -- returned a stuck 0x0f1c for every register as an isolated
 * loader command, while these same-file helpers (used by the selftest, which
 * reads real RSSI live) work.  Likely a per-TU codegen difference in the
 * timing-sensitive bit-bang.  Use the proven path. */
extern "C" uint16_t hd2_at1846s_read(uint8_t reg)
{
    return at1846s_read_reg(reg);
}
extern "C" void hd2_at1846s_write(uint8_t reg, uint16_t val)
{
    at1846s_write_reg(reg, val);
}

/*
 * One-shot full AT1846S RX bring-up from firmware (loader op 'I', at_reinit):
 * chip init() (vendor sequence + GDx supplement; ~700 ms of calibration
 * delays -- BLOCKS the calling diag thread that long) + FM filter bank 0 +
 * retune + vendor FM-RX reg-0x30 enable + the RX AF-DSP audio config.  Best
 * run with rf_freeze=1 so the rtx thread doesn't interleave bit-bang traffic
 * mid-init (the GPIOA bus has no real lock; see the q/Q caution in
 * hd2_diag.cpp).  freq_hz: the RX frequency to re-apply (the diag op passes
 * the rtx thread's current config).
 */
extern "C" void hd2_at1846s_reinit(uint32_t freq_hz)
{
    AT1846S& at = AT1846S::instance();

    at.init();                                /* vendor chip init + VCO cal  */
    at.setBandwidth(AT1846S_BW::_25);         /* FM bank 0 (vendor-confirmed) */
    at.setFrequency(freq_hz);                 /* PLL retune (leaves 0x30=0x4046) */

    /* Vendor FM-RX reg-0x30 enable pattern (rf_apply_channel_to_pll FM
     * branch): 0x4806 then 0x4826 = band + RX-side path bit11 + RX_ON. */
    at1846s_write_reg(0x30, 0x4806);
    at1846s_write_reg(0x30, 0x4826);

    /* RX AF-DSP listen config (re-asserts 0x4806/0x4826 at its tail). */
    hd2_at1846s_rx_audio_config();

    /* The chip is fully inited now -- keep the 'a' selftest from running a
     * second cold init on its next use. */
    g_radio_test_inited = true;
}

/*
 * A/B audio-gain profile switch, live (loader op 'o', at_profile):
 *   0 = vendor HD2 values (what init()+rx_audio_config program),
 *   1 = the GD77 driver's values for the same chip (working FM audio there).
 * Writes ONLY these four registers -- no other state is touched, so it can be
 * flipped while listening.  0x44 vendor form is 0x0900 | volume (g_fm_volume,
 * host-pokeable), matching hd2_at1846s_rx_audio_config.
 */
extern "C" void hd2_at1846s_profile(uint32_t profile)
{
    if (profile == 0u)
    {
        at1846s_write_reg(0x0a, 0x4c20);     /* PGA gain        (vendor) */
        at1846s_write_reg(0x59, 0x0b90);     /* mixer gain      (vendor) */
        at1846s_write_reg(0x33, 0x44a5);     /* AGC number      (vendor FM-RX) */
        at1846s_write_reg(0x44, (uint16_t)(0x0900u | (g_fm_volume & 0xffu)));
    }
    else
    {
        at1846s_write_reg(0x0a, 0x7ba0);     /* PGA gain        (GD77) */
        at1846s_write_reg(0x59, 0x09d2);     /* mixer gain      (GD77) */
        at1846s_write_reg(0x33, 0x45f5);     /* AGC number      (GD77) */
        at1846s_write_reg(0x44, 0x05cc);     /* TX/RX gain      (GD77 final) */
    }
}

/*
 * Set/clear AT1846S reg 0x30 bit7 -- the documented RX AF mute (loader op
 * 'm', at_mute).  Read-modify-write so the band/RX-enable bits are kept.
 * Returns the resulting reg 0x30 value (post-write readback).
 */
extern "C" uint16_t hd2_at1846s_afmute(uint32_t mute)
{
    uint16_t r = at1846s_read_reg(0x30);
    if (mute != 0u) r |= (uint16_t)0x0080u;
    else            r &= (uint16_t)~0x0080u;
    at1846s_write_reg(0x30, r);
    return at1846s_read_reg(0x30);
}
