/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenRTX radio.h driver for the Ailunce HD2 (HR_C7000 / CSKY V2 ck803s).
 *
 * SCOPE: analog-FM RX control path + FM voice TX (2026-06-11).
 *   - tune (radio_setVcoFrequency-equivalent via radio_enableRx)
 *   - RSSI read (AT1846S reg 0x1B)
 *   - CTCSS/DCS digital squelch (AT1846S reg 0x3A/0x1C)
 *   - opmode / opstatus bookkeeping
 *
 * The RF squelch decision lives in the OpenRTX core (OpMode_FM); this driver
 * surfaces the AT1846S hardware carrier/squelch comparator through the
 * radio_checkRxRfSquelch() hook (steadier than an RSSI threshold), plus a
 * calibrated RSSI and the tone-squelch flag.
 *
 * Modeled on platform/drivers/baseband/radio_GDx.cpp (the other
 * AT1846S-direct OpenRTX target).  This FM-only bring-up build strips the DMR
 * modem path (HR_C7000 Layer-2) and the diagnostic/loader helper surface; only
 * the FM RX + FM voice-TX entry points remain.
 *
 * Vendor V2.1.3 references (app image @ 0x0300d000):
 *   - at1846s_rx_path_open   @ 0x030405a0 : reg0x30 |= 0x20 ; gpio_out_set(0x24)
 *   - at1846s_rx_path_close  @ 0x03040564 : reg0x30 &= ~0x20 ; spkr unmute
 *   - rx_squelch_monitor_tick@ 0x03040a6c : reads reg 0x1C bit0 = carrier/sql
 *   - FUN_03040994           @ 0x03040994 : reads reg 0x1B, (v & 0x7FFF) >> 8 = RSSI
 *   - at1846s_set_freq       @ 0x03058e24 : ported in AT1846S_HD2.cpp
 */

#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include "drivers/baseband/AT1846S.h"
#include "drivers/i2c1_hd2.h"
#include "drivers/GPIO/gpio_hrc7000.h"
#include "radioUtils.h"

// Exact-value AT1846S writes for the TX key/dekey sequence (this file, below);
// TX was brought up and verified with these exact words -- keep them
// bit-identical rather than routing through the AT1846S class setters.
extern "C" void hd2_at1846s_write(uint8_t reg, uint16_t val);
extern "C" uint16_t hd2_at1846s_read(uint8_t reg);

// AT1846S RX-audio (AF-DSP) listen config -- proven path lives in this file
// (below).  Applied once per RX entry.
extern "C" void hd2_at1846s_rx_audio_config(void);

// ONE-TIME modem RX + baseband-IF-ADC + codec bring-up (this file, below).
// Called once from radio_init (before any RX); does the SYS_SOFT_RSTN modem
// reset (narrowed to spare the CPU ADC) + codec + modem FM gate config.
extern "C" void hd2_modem_fm_boot_init(void);

// RF-freeze flag (this file, below).  While set, every radio_* entry point that
// would touch the AT1846S skips its chip I/O so a host-side live experiment
// isn't overwritten.  radio_getRssi returns the last cached value while frozen.
extern "C" {
extern volatile uint32_t g_rf_freeze;
}

/* Deferred AT1846S RX-AF mute request.  Set (=1) / cleared (=0) by
 * audio_connect/disconnect(SOURCE_MCU, SINK_SPK) on ANY thread -- a plain memory
 * write, no I2C.  radio_getRssi(), on the rtx thread, reconciles it into the
 * chip's reg-0x30 bit7 AF mute so MCU/PCM playback isn't buried by the live
 * analog FM-RX audio, which mixes into the shared speaker node. */
extern "C" {
volatile uint8_t g_rx_af_mute_req = 0;
}

static const rtxStatus_t *config;  // Radio configuration pointer
static enum opstatus radioStatus;  // Current operating status
static Band currRxBand = BND_NONE; // Current RX band

/*
 * APC TX-power level (CPU DAC channel B, 12-bit; vendor ramps this on
 * every TX entry from a per-band power cal table we don't parse yet).
 * Conservative default: quarter scale.
 */
static volatile uint16_t apcLevel = 0x400;

/*
 * Map the current codeplug TX power (mW) to the two hardware drive controls:
 * APC DAC level (ch B) and AT1846S reg 0x0a padrv field.  Four discrete levels
 * matching the UI (Extra Low / Low / Medium / High).  Values are conservative --
 * even "High" sits well below the DAC ceiling (the PA runs hot near full drive).
 */
extern "C" void hd2_txpower_levels(uint16_t *apc, uint16_t *padrv)
{
    const uint32_t p = (config != nullptr) ? config->txPower : 1000u;
    if (p <= 100u) {
        *apc = 0x060u;
        *padrv = 0x08u;
    } /* Extra Low */
    else if (p <= 1000u) {
        *apc = 0x100u;
        *padrv = 0x08u;
    } /* Low       */
    else if (p <= 2500u) {
        *apc = 0x200u;
        *padrv = 0x0Fu;
    } /* Medium    */
    else {
        *apc = 0x400u;
        *padrv = 0x0Fu;
    } /* High      */
}

static AT1846S &at1846s = AT1846S::instance(); // AT1846S driver (singleton)

void radio_init(const rtxStatus_t *rtxState)
{
    config = rtxState;
    radioStatus = OFF;

    /*
     * Bring up the AT1846S.  init() runs the full vendor V2.1.3
     * chip-init + VCO calibration dance and leaves the chip configured
     * for the FM (25 kHz) audio bank with RX and TX both off.
     *
     * The I2C transport (I2C1, SCL=PTA7 / SDA=PTA8, slave 0xE2) is
     * initialised by the AT1846S constructor via i2c0_init(); no extra
     * wiring is needed here.
     */
    at1846s.init();

    // ONE-TIME HR_C7000 modem RX + baseband-IF-ADC + codec bring-up, here at init
    // (before any RX, on the rtx thread at startup).  This is the ONLY place the
    // modem SYS_SOFT_RSTN reset + modem FM gate config happen -- doing them per RX
    // entry (radio_enableRx) hung the bus (2026-06-09).  The reset is narrowed to
    // spare the CPU/volume ADC the UI is already using.
    hd2_modem_fm_boot_init();

    // Keep AF output muted until the core opens the squelch.
    radio_disableAfOutput();
}

void radio_terminate()
{
    radioStatus = OFF;
    radio_disableRtx();
}

void radio_setOpmode(const enum opmode mode)
{
    if (g_rf_freeze != 0u)
        return; /* rf_freeze: no AT1846S writes */

    switch (mode) {
        case OPMODE_FM:
            at1846s.setOpMode(AT1846S_OpMode::FM);
            break;

        default:
            /* DMR / M17 / broadcast-FM are out of scope for this FM-only
             * build; leave the transceiver as-is. */
            break;
    }
}

bool radio_checkRxDigitalSquelch()
{
    // CTCSS tone detection via AT1846S reg 0x3A/0x1C.  The upstream rtxStatus_t
    // carries only a CTC/DCS tone value (rxTone), not a tone TYPE field, so this
    // FM build follows radio_GDx.cpp and treats the sub-audio squelch as CTCSS.
    return at1846s.rxCtcssDetected();
}

void radio_enableAfOutput()
{
    /*
     * Unmute the AT1846S RX audio output (reg 0x30 bit 7).  The actual speaker
     * amp / audio routing on the HD2 is a board-level concern owned by
     * audio_HD2.c (PTB4/PTB10/PTB17); here we only release the chip-side mute.
     */
    if (g_rf_freeze != 0u)
        return; /* rf_freeze: no AT1846S writes */
    at1846s.unmuteRxOutput();
}

void radio_disableAfOutput()
{
    if (g_rf_freeze != 0u)
        return; /* rf_freeze: no AT1846S writes */
    at1846s.muteRxOutput();
}

void radio_enableRx()
{
    if (g_rf_freeze != 0u) /* rf_freeze: skip tune + AF-DSP */
        return;

    if (currRxBand == BND_NONE)
        return;

    // Tune the AT1846S VCO to the RX frequency and enable the RX path.
    at1846s.setFrequency(config->rxFrequency);
    at1846s.setFuncMode(AT1846S_FuncMode::RX);

    // Enable RX-side CTCSS detection if the channel requests it (CTCSS only --
    // the upstream config has no tone-type field; see radio_GDx.cpp).
    if (config->rxToneEn)
        at1846s.enableRxCtcss(config->rxTone);

    // Apply the AT1846S RX-audio (AF-DSP) config ONCE here -- the heavy I2C
    // burst belongs at RX entry, not on every squelch crossing.  Then release
    // the chip-side AF mute (reg 0x30 bit7) so the board speaker-amp (PTB4),
    // toggled by the audio matrix, is the only per-squelch gate.
    hd2_at1846s_rx_audio_config();
    at1846s.unmuteRxOutput();

    // The HR_C7000 modem RX + codec FM bring-up is done ONCE in radio_init
    // (hd2_modem_fm_boot_init), NOT here -- per-RX-entry modem writes/resets hung
    // the bus (2026-06-09).  radio_enableRx is AT1846S-only: tune + AF-DSP config
    // + chip unmute; the board speaker-amp (PTB4) is the per-squelch gate.

    radioStatus = RX;
}

/* ---------------------------------------------------------------------- *
 *  Canonical analog-FM TX key / unkey -- the SINGLE register recipe.
 *
 *  Does NOT select the modulator source: the OpMode path leaves the FM engine
 *  on the mic ADC (mic -> codec ADC -> C7000 FM modulator -> MOD1/MOD2 ->
 *  AT1846S varactors).  `narrow` picks the reg-0x59 deviation bank (true =
 *  12.5 kHz).  Do NOT touch SIG_CENTER/RF_MOD_BIAS -- the boot cal modulates
 *  cleanest (mid-scale guesses audibly degraded the audio).
 * ---------------------------------------------------------------------- */
extern "C" void hd2_fm_tx_key(uint32_t txFreq, bool narrow, uint8_t txToneEn,
                              uint16_t txTone)
{
    // Tune the VCO to the TX frequency -- RAW (2026-06-18).  Residual synth
    // error becomes a proper per-band/per-radio CALIBRATION (shared FM + DMR)
    // once nvm_readCalibData has an HD2 backend -- not a magic constant here.
    at1846s.setFrequency(txFreq);

    // Arm the C7000 FM-TX engine.  SYS_INTERP_MASK must hold the vendor FM value:
    // with the boot 0x7f mask the engine wedges on its first unserviced
    // FM_TX_INTERP and the carrier stays dead-quiet.  Masking (bit16) or acking
    // 0x3b0 works; we mask (no service thread).
    SOCSYS->AF_GATE = AF_GATE_FM_VENDOR;
    SOCSYS->WORK_MODE |= WORK_MODE_FM_MOD;
    SOCSYS->FM_PTT = 1u;

    // Band-select switch (PTB19, from the vendor spkr/mic_path_set_by_freq: high
    // band -> set, low band -> clear).  Steers the RF path / PA chain.
    if (txFreq >= 300000000u)
        gpio_setPin(GPIOB, 19);
    else
        gpio_clearPin(GPIOB, 19);

    // TX power level -> APC DAC drive (ch B) + AT1846S reg 0x0a padrv.
    uint16_t apc, padrv;
    hd2_txpower_levels(&apc, &padrv); // map current txPower -> APC + padrv
    apcLevel = apc;                   // (apcLevel is volatile; copy via temp)

    DAC_HW->PD_MODE_EN &= ~0x2u;      // ch B low-power mode off
    DAC_HW->PD_CTRL &= ~0x2u;         // ch B power up
    DAC_HW->DATA_B = apcLevel;

    // Vendor TX parity: mic-path gate PTB3 high (vendor mic_path_set_by_freq).
    gpio_setPin(GPIOB, 3);

    hd2_at1846s_write(0x0a, (uint16_t)((padrv << 11) | 0x0420u));

    // TX FM deviation (reg 0x59 -- shared with the RX mixer gain, restored in
    // hd2_fm_tx_unkey).  Pick by bandwidth and whether a sub-audio tone is
    // active (the low 6 bits set the CTCSS/DCS deviation).
    {
        uint16_t dev59;
        if (narrow)
            dev59 = txToneEn ? 0x0B11u : 0x0C90u;
        else
            dev59 = txToneEn ? 0x0C62u : 0x0C50u;
        hd2_at1846s_write(0x59, dev59);
    }

    // AT1846S: TX-side AF-DSP ctrl, then key the carrier (exact words from the
    // verified bring-up), then the vendor tx_pa_enable (reg 0x30 |= 0x80 -- the
    // datasheet calls bit7 "mute", but the vendor sets it on every key-up and
    // clears it on every dekey: it is the PA-stage gate in TX context).
    hd2_at1846s_write(0x40, 0x0030u);
    hd2_at1846s_write(0x30, 0x4006u);
    hd2_at1846s_write(0x30, 0x4046u); // tx_on
    hd2_at1846s_write(0x30, 0x40c6u); // + bit7: PA on (vendor tx_pa_enable)

    // Sub-audio encode: the AT1846S sums its own CTCSS generator into the TX
    // modulation; disableCtcss() in radio_disableRtx() clears it.  CTCSS only --
    // the upstream config carries no tone-type/tail-elim fields (see the port
    // notes); DCS-TX + reverse-burst tail elimination from the vendor path are
    // not wired in this build.
    if (txToneEn)
        at1846s.enableTxCtcss(txTone);
}

extern "C" void hd2_fm_tx_unkey(void)
{
    DAC_HW->DATA_B = 0u;              // APC drive to zero first
    hd2_at1846s_write(0x30, 0x4046u); // PA off (bit7 clear) first...
    hd2_at1846s_write(0x30, 0x4006u); // ...then tx_on off (dekey)
    hd2_at1846s_write(0x0a, 0x4c20u); // restore padrv to chip-init default
    DAC_HW->PD_CTRL |= 0x2u;          // APC DAC ch B power down
    gpio_clearPin(GPIOB, 3);          // mic gate off
    SOCSYS->FM_PTT = 0u;
    SOCSYS->WORK_MODE &= ~WORK_MODE_FM_MOD;
    hd2_at1846s_write(0x40, 0x0031u); // RX-side AF-DSP ctrl value
    hd2_at1846s_write(0x59,
                      0x0b90u); // restore RX mixer gain (TX set 0x59 deviation)
}

void radio_enableTx()
{
    /*
     * FM voice TX -- HW-verified 2026-06-11 (voice received on a second
     * radio; brought up via diag op 'Y').
     *
     * Architecture: mic -> codec ADC -> C7000 FM modulator engine ->
     * MOD1/MOD2 pins (two-point modulation) -> AT1846S varactors.  The
     * AT1846S only keys the carrier (its FM bank parks voice_sel=000);
     * the mic feed into the engine is pure hardware -- no CPU sample
     * pumping.  Do NOT touch SIG_CENTER/RF_MOD_BIAS: the boot/reset cal
     * modulates cleanest.
     */
    if (g_rf_freeze != 0u)
        return;

    if (config->txDisable == 1)
        return;

    // FM voice TX only (DMR / M17 out of scope for this build).
    if (config->opMode != OPMODE_FM)
        return;

    // AT1846S hardware band sanity (134-174 / 400-527 MHz).
    if (getBandFromFrequency(config->txFrequency) == BND_NONE)
        return;

    // Key the analog-FM TX path via the shared canonical recipe (the SINGLE
    // register sequence -- see hd2_fm_tx_key).
    hd2_fm_tx_key(config->txFrequency, config->bandwidth == BW_12_5,
                  config->txToneEn, config->txTone);

    radioStatus = TX;
}

void radio_disableRtx()
{
    if (g_rf_freeze != 0u) /* rf_freeze: no AT1846S writes */
    {
        radioStatus = OFF; /* keep the bookkeeping honest  */
        return;
    }

    /*
     * TX teardown first (no-op when not transmitting): dekey the AT1846S
     * carrier, then stop the C7000 FM-TX engine and drop back to the
     * RX-side work_mode.  Order mirrors the verified bring-up: carrier
     * off before the modulator so we don't radiate an unmodulated blip.
     */
    if (radioStatus == TX)
        hd2_fm_tx_unkey(); // shared canonical FM-TX teardown

    // Drop both RX and TX on the chip side and silence any tone output.
    at1846s.disableTone();
    at1846s.disableCtcss();
    at1846s.setFuncMode(AT1846S_FuncMode::OFF);
    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    if (g_rf_freeze != 0u) /* rf_freeze: no AT1846S writes */
        return;

    currRxBand = getBandFromFrequency(config->rxFrequency);

    if (currRxBand == BND_NONE)
        return;

    /*
     * Analog-FM bandwidth select.  No flash calibration backend yet, so the
     * per-band noise/RSSI/squelch threshold tuning that GDx pulls from calData
     * is left at the AT1846S init defaults.
     */
    if (config->opMode == OPMODE_FM) {
        switch (config->bandwidth) {
            case BW_12_5:
                at1846s.setBandwidth(AT1846S_BW::_12P5);
                break;

            case BW_25:
                at1846s.setBandwidth(AT1846S_BW::_25);
                break;

            default:
                break;
        }

        /*
         * Squelch is handled in software by OpMode_FM (RSSI threshold on
         * radio_getRssi()); the AT1846S internal RSSI squelch (reg 0x49) is
         * left at its init default rather than driven from config->sqlLevel.
         */
    }

    /*
     * Re-apply the VCO frequency / RX path if we are already receiving, so a
     * config change (e.g. a new RX frequency) takes effect without the core
     * having to cycle the op-status.  Mirrors radio_GDx.cpp.
     */
    if (radioStatus == RX)
        radio_enableRx();
}

rssi_t radio_getRssi()
{
    /*
     * AT1846S reg 0x1B upper byte holds the RSSI level; the generic driver maps
     * it to dBm as (-137 + (reg >> 8)).
     *
     * rf_freeze: even a *read* is bus traffic that can corrupt a concurrent
     * host transaction, so return the last cached value while frozen.
     *
     * Peak-hold (instant rise, slow fall ~2 dB/call @ ~33 Hz): with NO carrier
     * the chip refreshes reg-0x1B rssi_db only periodically and reads near-zero
     * between updates (HW 2026-06-14), so a plain read flickers the S-meter to
     * 0.  Peak-hold latches the periodic true value and ignores the stale dips,
     * while a real signal drop still falls.
     */
    static rssi_t held = -127;

    if (g_rf_freeze == 0u) {
        /* Reconcile the deferred RX-AF mute (see g_rx_af_mute_req) via the
         * AT1846S reg-0x30 bit7 AF mute -- the documented mute that cuts the
         * demodulated audio at the source even with the squelch open.
         * Re-assert every tick while a prompt holds MCU->SPK; unmute once when
         * the path is released. */
        static bool afMuted = false;
        if (g_rx_af_mute_req != 0u) {
            if (!afMuted) {
                at1846s.muteRxOutput();
                afMuted = true;
            } /* once, on 0->1 */
        } else if (afMuted) {
            at1846s.unmuteRxOutput();
            afMuted = false;
        }

        rssi_t raw = static_cast<rssi_t>(at1846s.readRSSI());
        if (raw >= held)
            held = raw;
        else if ((held - raw) > 2)
            held = static_cast<rssi_t>(held - 2);
        else
            held = raw;
    }

    return held;
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}

/* ============================================================== *
 *  AT1846S live register access + analog-FM audio/modem bring-up *
 * ============================================================== */
/* AT1846S I2C slave address (== 0x71 << 1), matches AT1846S_HD2.cpp. */
static constexpr uint8_t AT1846S_ADDR = 0xE2;

/*
 * RX audio volume (AT1846S reg 0x44 low byte; reg = 0x900 | vol).  Vendor
 * sources this from the codeplug tune data; our FM bring-up has no volume
 * setting, so it defaults near-max. */
extern "C" {
volatile uint32_t g_fm_volume = 0x3fu;
}

/*
 * RF FREEZE.  While 1, ALL firmware-initiated AT1846S I2C traffic and
 * audio-GPIO rewrites are suspended so a host can run live chip experiments
 * without the firmware overwriting them.  Gated call sites: radio_* here,
 * audio_connect/disconnect in audio_HD2.c, and the rtx task. */
extern "C" {
volatile uint32_t g_rf_freeze = 0u;
}

/*
 * Read one 16-bit AT1846S register over the I2C1 bus, using the exact same
 * primitives + big-endian byte order as AT1846S::i2c_readReg16.
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
 *
 * CODEC_BYTE(off) comes from registers.h (byte-addressed PCM block); keep the
 * short local CB() name this driver uses.
 */
#define CB(off) CODEC_BYTE(off)

/* Codec DAC output gain (GCR_DACL @ codec 0xdf): vendor sources the low 6 bits
 * (godl) from codeplug calibration; our FM build has no codeplug, so use the
 * vendor's known-good value (= g_tune_data 0x34 | 0x80, from a vendor unit
 * playing FM) so the codec DAC isn't mis-gained. */
#define CODEC_DACL_GAIN 0xb4u

/* NOTE on the codec bring-up below: SOCSYS->SYS_SOFT_RSTN is pulsed to reset the
 * codec block, and SOCSYS->LINEOUT_CTRL (0x88) is read as the codec-DAC-enable
 * handshake flag (bit31 standby_lo) -- both accessed directly on the struct. */

static void delay_ms(unsigned ms)
{
    for (unsigned m = 0; m < ms; ++m)
        for (volatile unsigned i = 0; i < 8000u; ++i) {
        }
}

static bool g_codec_inited = false;

static void hd2_codec_audio_init(void)
{
    /* PCM block reset pulse + codec soft-reset (matches vendor exactly). */
    CB(0xd2) |= 0x03;
    SOCSYS->SYS_SOFT_RSTN &= 0xffffffefu; /* clear bit4 = codec reset */
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
        if ((SOCSYS->LINEOUT_CTRL & 0x80000000u) == 0u)
            break;
        delay_ms(10);
    }

    CB(0xcd) &= ~0x80;
    delay_ms(100);
    CB(0xdf) = CODEC_DACL_GAIN;
    CB(0xe5) = 0x8b;
    SOCSYS->LINEOUT_CTRL = 1;

    /* boot_init_audio_route analog tail (same block). */
    CB(0xc8) = 0xc0;
    CB(0xcd) = 0x20;
    CB(0xe5) = 0x8b;
    CB(0xdf) = CODEC_DACL_GAIN;
    SOCSYS->PCM_MODE |= 0x02; /* _hrc7000_pcm_mode |= 2 */
}

/*
 * Warm up the HR_C7000 codec audio-OUTPUT path ONCE: codec bring-up + the socsys
 * audio gate, with NO AT1846S RF tune and NO modem-RX servicing loop.  The
 * PWM-beep mixes THROUGH the codec (DAC -> lineout -> speaker), so the codec
 * must be initialised before a beep is audible.  Called lazily from
 * platform_beepStart.  GPIOB.4/.10 (amp + route) are left to the caller so they
 * re-assert per beep.
 */
extern "C" void hd2_audio_out_warm(void)
{
    static bool warmed = false;
    if (warmed)
        return;
    if (!g_codec_inited) {
        hd2_codec_audio_init();
        g_codec_inited = true;
    }
    SOCSYS->DAC_CONTROL = 0x8000001fu;
    SOCSYS->ADC_CONTROL = 0x000041c3u;
    SOCSYS->VOICE_PATH = 0x00000002u;
    SOCSYS->PCM_MODE = 0x00000000u;
    SOCSYS->LINEOUT_CTRL =
        0x00000003u; /* both lineouts; speaker is on LINE2OUT */
    SOCSYS->WORK_MODE = 0x0000006eu;
    SOCSYS->AF_GATE = 0x0001007fu;
    SOCSYS->LAYER2_CONTROL = 0x000000c4u;
    SOCSYS->LAYER2_TXRX_CTRL = 0x00000040u;
    warmed = true;
}

/* Gate the codec-DAC lineout for MCU (beep / voice-prompt / PCM) playback.
 * hd2_audio_out_warm() latches, so once it has run the DAC + LINEOUT stay
 * enabled forever; a warm-but-idle DAC then leaks a faint analog noise floor
 * into the speaker amp (post the codec digital volume, so the volume knob can't
 * touch it).  The audio-path HAL calls this on every SOURCE_MCU->SINK_SPK
 * open/close so the DAC + lineout are live only while a clip is actually
 * playing -- no idle hiss between prompts.  Does NOT touch the AT1846S analog
 * FM-RX path (that reaches the amp on PTB17=HIGH, not through the codec DAC). */
extern "C" void hd2_audio_out_lineout(int on)
{
    if (on) {
        CB(0xdf) = CODEC_DACL_GAIN;         /* restore codec-chip DAC gain     */
        SOCSYS->DAC_CONTROL  = 0x8000001fu; /* bit5 clear -> SOCSYS DAC on      */
        SOCSYS->LINEOUT_CTRL = 0x00000003u; /* both lineouts (speaker=LINE2OUT) */
    } else {
        SOCSYS->LINEOUT_CTRL = 0x00000000u; /* lineout disable                  */
        SOCSYS->DAC_CONTROL  = 0x8000003fu; /* bit5 set   -> SOCSYS DAC standby */
        CB(0xdf) = 0x00u;                   /* mute codec-chip DAC gain         */
    }
}

/*
 * AT1846S RX-audio (AF-DSP) config -- the chip-side half of the proven listen
 * sequence, WITHOUT the codec/socsys gate and WITHOUT any board GPIO.  Call
 * ONCE per RX entry (radio_enableRx); it's a ~12-write I2C burst + a
 * setBandwidth filter-bank load, far too heavy to run on every squelch
 * crossing.
 */
extern "C" void hd2_at1846s_rx_audio_config(void)
{
    at1846s_write_reg(0x40, 0x0030); /* RX AF-DSP LISTEN (NOT 0x11 scan-mute) */

    at1846s_write_reg(0x41, 0x471e);
    at1846s_write_reg(0x44, 0x0900u | (uint16_t)(g_fm_volume & 0xff));
    at1846s_write_reg(0x33, 0x44a5);
    at1846s_write_reg(0x54, 0x2a3c);
    at1846s_write_reg(0x63, 0x16ad);
    at1846s_write_reg(0x58, 0x8405);
    at1846s_write_reg(0x4e, 0x6002);

    at1846s_write_reg(0x7a, 0xa00a);
    AT1846S::instance().setBandwidth(
        AT1846S_BW::_25); /* FM bank 0 (vendor-confirmed) */

    /* 0x3a (voice-channel/audio-path select) MUST come AFTER setBandwidth: the
     * FM bank table includes 0x3a=0x00c3 and silently clobbers the vendor FM-RX
     * value. */
    at1846s_write_reg(0x3a, 0x80e1);

    /* Vendor FM-RX reg-0x30 enable (rf_apply_channel_to_pll FM branch does
     * 0x4806 then 0x4826).  0x4826 = band(0x4000)+RX-side(0x800)+RX_ON(0x20)+0x06. */
    at1846s_write_reg(0x30, 0x4806);
    at1846s_write_reg(0x30, 0x4826);
}

/*
 * Complete analog-FM RX audio bring-up on the HR_C7000 side: codec DAC power-up
 * (once) + the modem analog-FM audio gate (socsys).  This is what lets the
 * AT1846S-IF FM demod reach the codec DAC -> lineout -> speaker.  Called ONCE
 * from radio_init.
 *
 * EVERY value here was verified LIVE against a vendor radio playing FM out the
 * speaker, by word-diffing its socsys/codec block over dbgshell (2026-06-09).
 */
extern "C" void hd2_modem_fm_boot_init(void)
{
    /* ONE-TIME modem RX + baseband-IF-ADC + codec bring-up.  Call ONCE from
     * radio_init (rtx thread startup), BEFORE any RX.
     *
     * Reset value 0x1d0 (SYS_SOFT_RSTN, active-low, auto-releasing) holds
     * protocol[0]+phy[1]+fm[2]+audio[3]+adc[5] in reset, RELEASES codec[4]+
     * adc_ctrl[6]+sys[7]+cpu[8].  This is the vendor's boot value 0x190 MINUS
     * bit6 (adc_ctrl): we must NOT reset the CPU/volume ADC at 0x140d0000 -- the
     * UI/main threads are already polling it.  bit5 (the baseband IF ADC) IS
     * reset -- that's the one the modem demod needs. */
    /* FM-RX CLOCK GATE (2026-06-10).  CLK_MGR @0x2c boot value 0xfff0ff3c leaves
     * [7]modem_clk_fmrx_en + [6]modem_clk_fmtx_en OFF -- so the analog-FM-RX
     * baseband datapath has NO CLOCK.  Blind write of boot-value|0xC0 (enable
     * both FM clocks): no read (modem-MMIO reads wedge our loader) and no clock
     * gets disabled, so safe from the rtx thread.  Must precede the reset +
     * datapath config below. */
    SOCSYS->CLK_MGR_REG2C = 0xfff0fffcu;

    SOCSYS->SYS_SOFT_RSTN = 0x000001d0u;

    /* Codec power-up (the audio block was just reset). */
    g_codec_inited = false;
    hd2_codec_audio_init();
    g_codec_inited = true;

    /* AF-RECEIVE BIAS (the fix, RE 2026-06-09): in AF mode (RF_MODE[24]=1) the
     * AT1846S audio enters the baseband ADC's VINP, and the ADC's VINM/VCM must
     * be biased by the CPU-bus DAC (manual §8.2.3.1).  Vendor dac_controller_init
     * @0x0305960C: route the DAC analog-out pins (DIPLEX2 PTC-mux bits 28/29
     * clear), power up DAC channels B+C, and drive channel C to the AF bias 0x6E2. */
    SOCSYS->IO_DIPLEX2 &=
        0xCFFFFFFFu; /* route DAC out pins (clear PTC mux bits 28-29) */
    DAC_HW->PD_MODE_EN = 0x00000001u;
    DAC_HW->DATA_A = 0x00000000u;
    DAC_HW->DATA_B = 0x00000000u;
    DAC_HW->DATA_C = 0x00000000u;
    DAC_HW->PD_CTRL = 0x00000001u; /* power-down A (bit0=1), power-up B+C */
    DAC_HW->DATA_C =
        0x000006E2u; /* AF-receive single-point bias (vendor value) */

    /* Modem analog-FM datapath gate + RF/IF/AGC -- written ONCE here, after the
     * reset.  Values verified live against a vendor radio playing FM. */
    SOCSYS->DAC_CONTROL = 0x8000001fu;
    SOCSYS->ADC_CONTROL =
        0x000041c3u; /* enadc0[8]+enadc1[7]+adc_enref[6] (baseband IF ADC) */
    SOCSYS->VOICE_PATH = 0x00000002u;   /* bit0=0 -> audio via codec DAC (FM) */
    SOCSYS->PCM_MODE = 0x00000000u;
    SOCSYS->LINEOUT_CTRL = 0x00000001u; /* line2out only -- vendor live value */
    SOCSYS->WORK_MODE = 0x0000006eu;    /* FM-analog (vendor live) */
    SOCSYS->RF_MODE = 0x034c9060u;      /* RF_MODE: AF-receive (vendor live) */
    SOCSYS->RF_CONTROL = 0x00041f1au;   /* RF_CONTROL (vendor live) */
    SOCSYS->RF_MOD_BIAS =
        0x01e80000u; /* RF/IF reg (vendor live; was 0 on ours) */
    SOCSYS->THRESHOLD_VALUE =
        0x0978786fu; /* arrival/timing-sync detect (vendor live) */
    SOCSYS->SLOT_GUARD = 0x00000014u;     /* slot_guard (vendor live) */
    SOCSYS->RX_IF_FREQ = 0x000bb800u;     /* RX_IF_FREQ = 768 kHz */
    SOCSYS->RX_AGC = 0x000036b0u;         /* RX_AGC (vendor live) */
    SOCSYS->AF_GATE = 0x0001007fu;        /* FM-RX audio gate (vendor live) */
    SOCSYS->LAYER2_CONTROL = 0x000000c4u; /* modem RX datapath shadow */
    SOCSYS->LAYER2_TXRX_CTRL = 0x00000040u;
}

/* Live AT1846S register access, used by the TX key/dekey sequence above
 * (hd2_fm_tx_key writes reg 0x0a/0x30/0x40/0x59 with exact verified words). */
extern "C" uint16_t hd2_at1846s_read(uint8_t reg)
{
    return at1846s_read_reg(reg);
}
extern "C" void hd2_at1846s_write(uint8_t reg, uint16_t val)
{
    at1846s_write_reg(reg, val);
}

/*
 * RF carrier/squelch detect via the AT1846S's OWN comparator: reg 0x1C bit0 =
 * sq_cmp, the chip's RSSI+noise decision with built-in hi/lo hysteresis (using
 * the chip's init-default thresholds).  Much steadier than thresholding our raw
 * reg-0x1B RSSI.  Mirrors the vendor's rx_squelch_monitor_tick (@0x03040a6c).
 * Exposed to OpMode_FM via the radio_checkRxRfSquelch() strong override below.
 */
extern "C" bool hd2_rx_carrier_detected(void)
{
    if (g_rf_freeze != 0u)
        return false;
    return ((at1846s_read_reg(0x1C) & 0x0001u) != 0u); // sq_cmp
}

/*
 * Strong override of the OpMode_FM hardware RF-squelch hook (radio.h): the HD2
 * has a real on-chip squelch comparator, so report sq_cmp instead of letting
 * OpMode_FM threshold the jittery raw RSSI.
 */
extern "C" bool radio_checkRxRfSquelch(bool *open)
{
    *open = hd2_rx_carrier_detected();
    return true;
}
