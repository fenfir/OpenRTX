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
 * The RF squelch decision itself lives in the OpenRTX core (OpMode_FM),
 * which turns rtx_getRssi() + sqlLevel into an open/close decision; the
 * driver only has to surface a calibrated RSSI and the tone-squelch flag.
 *
 * Modeled on platform/drivers/baseband/radio_GDx.cpp (the other
 * AT1846S-direct OpenRTX target).  Differences vs GDx:
 *   - No HR_C6000 baseband modem (GDx drives DMR/audio via HR_C6000; on
 *     the HD2 that path is the separate HR_C7000 modem -- DMR deferred).
 *   - No MK22 DAC / APC voltage / PA-drive: TX is intentionally disabled
 *     in this RX-only bring-up, so the TX power chain is a no-op.
 *   - No GDx flash calibration blob: nvm_readCalibData() has no HD2
 *     backend yet (codeplug/W25Q parked, #73).  Squelch / noise / RSSI
 *     thresholds use the AT1846S power-on defaults from at1846s.init()
 *     until a calibration source lands.
 *   - Board-level RX-enable GPIO (vendor at1846s_rx_path_open does
 *     gpio_out_set(0x24) == GPIOB.4) is NOT driven here: the HD2 OpenRTX
 *     GPIO driver only exposes GPIOA/GPIOC today (gpio_csky.h).  RX is
 *     enabled chip-side via AT1846S reg 0x30 bit 5 (setFuncMode(RX)),
 *     which is sufficient for the AT1846S to demodulate; wiring the
 *     GPIOB RX/audio-enable is a TODO for the audio agent.
 *
 * Vendor V2.1.3 references (app image @ 0x0300d000):
 *   - at1846s_rx_path_open   @ 0x030405a0 : reg0x30 |= 0x20 ; gpio_out_set(0x24)
 *   - at1846s_rx_path_close  @ 0x03040564 : reg0x30 &= ~0x20 ; spkr unmute
 *   - rx_squelch_monitor_tick@ 0x03040a6c : reads reg 0x1C bit0 = carrier/sql
 *   - FUN_03040994           @ 0x03040994 : reads reg 0x1B, (v & 0x7FFF) >> 8 = RSSI
 *   - at1846s_set_freq       @ 0x03058e24 : ported in AT1846S_HD2.cpp
 */

#include "interfaces/radio.h"
#include "drivers/baseband/AT1846S.h"
#include "radioUtils.h"
#include "hd2_regs.h"

// Exact-value AT1846S writes for the TX key/dekey sequence (radio_test_HD2.cpp;
// same helper the proven diag op 'Y' uses).  The AT1846S class API would give
// near-equivalent values via setFuncMode, but TX was brought up and verified
// with these exact words -- keep them bit-identical.
extern "C" void     hd2_at1846s_write(uint8_t reg, uint16_t val);
extern "C" uint16_t hd2_at1846s_read(uint8_t reg);

// AT1846S RX-audio (AF-DSP) listen config -- proven path lives in
// radio_test_HD2.cpp (file-static bit-bang helper).  Applied once per RX entry.
extern "C" void hd2_at1846s_rx_audio_config(void);

// ONE-TIME modem RX + baseband-IF-ADC + codec bring-up (radio_test_HD2.cpp).
// Called once from radio_init (before any RX); does the SYS_SOFT_RSTN modem
// reset (narrowed to spare the CPU ADC) + codec + modem FM gate config.
extern "C" void hd2_modem_fm_boot_init(void);

// RF-freeze flag (radio_test_HD2.cpp, loader op 'z').  While set, every
// radio_* entry point that would touch the AT1846S skips its chip I/O so a
// host-side live experiment isn't overwritten.  rtx_task() is already gated
// at the top (hd2_rtx.c); the guards here are defense-in-depth for any other
// caller.  radio_getRssi returns the last cached value while frozen.
extern "C" { extern volatile uint32_t g_rf_freeze; }

// FM-TX carrier lands 12.5 kHz low of commanded (HW-measured 2026-06-12);
// correct it on the TX tune.  See radio_enableTx.
#define HD2_FM_TX_FREQ_OFFSET_HZ  12500

static const rtxStatus_t *config;                // Radio configuration pointer
static enum opstatus       radioStatus;          // Current operating status
static Band                currRxBand = BND_NONE; // Current RX band

/*
 * APC TX-power level (CPU DAC channel B, 12-bit; vendor ramps this on
 * every TX entry from a per-band power cal table we don't parse yet).
 * Tunable live between key-ups via the router target RT 16 ('S' op).
 * Conservative default: quarter scale.
 */
static volatile uint16_t apcLevel = 0x400;

extern "C" void     hd2_apc_set(uint16_t level) { apcLevel = level & 0xfff; }
extern "C" uint16_t hd2_apc_get(void)           { return apcLevel; }

static AT1846S& at1846s = AT1846S::instance();    // AT1846S driver (singleton)

void radio_init(const rtxStatus_t *rtxState)
{
    config      = rtxState;
    radioStatus = OFF;

    /*
     * Bring up the AT1846S.  init() runs the full vendor V2.1.3
     * chip-init + VCO calibration dance and leaves the chip configured
     * for the FM (25 kHz) audio bank with RX and TX both off.
     *
     * The I2C transport (GPIOA bit-bang, SCL=PTA7 / SDA=PTA8, slave
     * 0xE2) is initialised lazily by the AT1846S constructor via
     * i2c0_init(); no extra wiring is needed here.
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
    if(g_rf_freeze != 0u) return;          /* rf_freeze: no AT1846S writes */

    switch(mode)
    {
        case OPMODE_FM:
            at1846s.setOpMode(AT1846S_OpMode::FM);
            break;

        case OPMODE_DMR:
            /*
             * DMR on the HD2 is owned by the HR_C7000 modem (not ported
             * yet).  We at least program the AT1846S into its 12.5 kHz /
             * DMR filter bank so the chip side is consistent; the modem
             * datapath is a TODO.
             */
            at1846s.setOpMode(AT1846S_OpMode::DMR);
            at1846s.setBandwidth(AT1846S_BW::_12P5);
            break;

        default:
            break;
    }
}

bool radio_checkRxDigitalSquelch()
{
    // CTCSS/DCS tone detection via AT1846S reg 0x3A/0x1C.
    return at1846s.rxCtcssDetected();
}

void radio_enableAfOutput()
{
    /*
     * Unmute the AT1846S RX audio output (reg 0x30 bit 7).  NOTE: the
     * actual speaker amp / audio routing on the HD2 is a board-level
     * concern owned by the audio agent (vendor spkr_amp_unmute +
     * gpio_out_set(0x24)); here we only release the chip-side mute.
     */
    if(g_rf_freeze != 0u) return;          /* rf_freeze: no AT1846S writes */
    at1846s.unmuteRxOutput();
}

void radio_disableAfOutput()
{
    if(g_rf_freeze != 0u) return;          /* rf_freeze: no AT1846S writes */
    at1846s.muteRxOutput();
}

void radio_enableRx()
{
    if(g_rf_freeze != 0u)                  /* rf_freeze: skip tune + AF-DSP */
        return;

    if(currRxBand == BND_NONE)
        return;

    // Tune the AT1846S VCO to the RX frequency and enable the RX path.
    at1846s.setFrequency(config->rxFrequency);
    at1846s.setFuncMode(AT1846S_FuncMode::RX);

    // Enable RX-side CTCSS/DCS detection if the channel requests it.
    if(config->rxToneEn)
        at1846s.enableRxCtcss(config->rxTone);

    // Apply the AT1846S RX-audio (AF-DSP) config ONCE here -- the heavy I2C
    // burst belongs at RX entry, not on every squelch crossing.  Then release
    // the chip-side AF mute (reg 0x30 bit7) so the board speaker-amp (PTB4),
    // toggled by the audio matrix, is the only per-squelch gate.
    hd2_at1846s_rx_audio_config();
    at1846s.unmuteRxOutput();

    // The HR_C7000 modem RX + codec FM bring-up is done ONCE in radio_init
    // (hd2_modem_fm_boot_init), NOT here -- per-RX-entry modem writes/resets hung the
    // bus (2026-06-09).  radio_enableRx is AT1846S-only: tune + AF-DSP config + chip
    // unmute; the board speaker-amp (PTB4) is the per-squelch gate.

    radioStatus = RX;
}

void radio_enableTx()
{
    /*
     * FM voice TX -- HW-verified 2026-06-11 (voice received on a second
     * radio; brought up via diag op 'Y', see docs/audio_paths.md §5).
     *
     * Architecture: mic -> codec ADC -> C7000 FM modulator engine ->
     * MOD1/MOD2 pins (two-point modulation) -> AT1846S varactors.  The
     * AT1846S only keys the carrier (its FM bank parks voice_sel=000);
     * the mic feed into the engine is pure hardware -- no CPU sample
     * pumping.  Do NOT touch SIG_CENTER/RF_MOD_BIAS: the boot/reset cal
     * modulates cleanest (mid-scale guesses audibly degraded the audio,
     * and readback shows the RX bank, so values can't be verified).
     */
    if(g_rf_freeze != 0u)
        return;

    if(config->txDisable == 1)
        return;

    // FM only: DMR TX is a different (modem burst) path, not ported.
    if(config->opMode != OPMODE_FM)
        return;

    // AT1846S hardware band sanity (134-174 / 400-527 MHz).
    if(getBandFromFrequency(config->txFrequency) == BND_NONE)
        return;

    // Tune the VCO to the TX frequency, with a +12.5 kHz correction.
    //
    // HW-MEASURED 2026-06-12 (spectrum analyser): our FM-TX carrier sits a
    // fixed 12.5 kHz LOW of the commanded frequency -- a sibling HD2 (forgiving
    // front end) still decodes it, but a narrow-IF receiver rejects the
    // off-channel carrier and won't open squelch.  Dialing +12.5 kHz lands the
    // carrier dead on 430.000 and opens squelch on every receiver tested.
    // Applying the correction here so the channel frequency is honoured.
    //
    // NOTE: treated as a FIXED-Hz offset (option 2, 2026-06-12).  Not yet
    // confirmed whether the true cause is a fixed IF/programming constant
    // (offset constant across bands) or a reference/MOD-bias pull (offset
    // scales ~29 ppm with frequency) -- if a future band shows a different
    // error, revisit (the 145 MHz cross-check distinguishes them).
    at1846s.setFrequency(config->txFrequency + HD2_FM_TX_FREQ_OFFSET_HZ);

    /*
     * Arm the C7000 FM-TX engine.  SYS_INTERP_MASK must hold the vendor
     * FM value: with the boot 0x7f mask the engine wedges on its first
     * unserviced FM_TX_INTERP and the carrier stays dead-quiet.  Either
     * masking (bit16) or acking 0x3b0 works; we mask (no service thread).
     */
    SOCSYS_AF_GATE    = AF_GATE_FM_VENDOR;
    SOCSYS_WORK_MODE |= WORK_MODE_FM_MOD;
    SOCSYS_FM_PTT     = 1u;

    /*
     * Band-select switch (PTB19, live-decoded from the vendor IAP copies
     * of spkr/mic_path_set_by_freq: high band -> set, low band -> clear).
     * The vendor holds it high at 430 MHz in RX too; we set it per the TX
     * frequency here -- it steers the RF path/PA chain.
     */
    if(config->txFrequency >= 300000000u)
        gpio_atomic_set(&GPIOB_DR, (1u << 19));
    else
        gpio_atomic_clear(&GPIOB_DR, (1u << 19));

    /*
     * APC TX-power drive: CPU DAC channel B (the line the vendor ramps on
     * every PTT edge -- without it the PA sits at minimum drive and the
     * carrier barely crosses the room).  Power the channel up and set the
     * level; no soft ramp yet (vendor steps it over ~tens of ms for
     * splatter politeness -- TODO with the power cal table).
     */
    DAC_PD_MODE_EN &= ~0x2u;                   // ch B low-power mode off
    DAC_PD_CTRL    &= ~0x2u;                   // ch B power up
    DAC_DATA_B      = apcLevel;

    // Vendor TX parity: mic-path gate PTB3 high (vendor mic_path_set_by_freq).
    gpio_atomic_set(&GPIOB_DR, (1u << 3));

    /*
     * TX RF power = AT1846S reg 0x0a padrv_ibit (bits 14:11, "output of RF
     * power control"; vendor runtime expr (code & 0x1f)<<11 | 0x420, default
     * 0x7c20 = padrv 0xF max).  The low bits 0x420 hold pga_gain=0x10 (TX
     * voice analog gain) + pabias=0, matching the vendor analog-TX value.
     *
     * config->txPower is in mW.  Without an HD2 flash-cal backend yet (the
     * per-band tables at 0x7af280 the vendor interpolates), use a 2-level map
     * honouring the codeplug Low/High flag: High -> full padrv, Low -> backed
     * off.  Refine to a calibrated per-band curve when nvm_readCalibData has
     * an HD2 backend.  (reg 0x0a was wrongly dismissed during the 2026-06-11
     * power hunt -- that was a deaf bench RX, not an ineffective register.)
     */
    {
        uint16_t padrv = (config->txPower > 1500u) ? 0x0Fu   /* High: full  */
                                                   : 0x08u;  /* Low:  ~half */
        hd2_at1846s_write(0x0a, (uint16_t)((padrv << 11) | 0x0420u));
    }

    // AT1846S: TX-side AF-DSP ctrl, then key the carrier (exact words
    // from the verified bring-up sequence), then the vendor's
    // tx_pa_enable: reg 0x30 |= 0x80.  The datasheet calls bit7 "mute",
    // but the vendor's PA-enable function sets it on every key-up and
    // clears it on every dekey -- it is the PA-stage gate in TX context.
    hd2_at1846s_write(0x40, 0x0030u);
    hd2_at1846s_write(0x30, 0x4006u);
    hd2_at1846s_write(0x30, 0x4046u);          // tx_on
    hd2_at1846s_write(0x30, 0x40c6u);          // + bit7: PA on (vendor tx_pa_enable)

    radioStatus = TX;
}

void radio_disableRtx()
{
    if(g_rf_freeze != 0u)                  /* rf_freeze: no AT1846S writes */
    {
        radioStatus = OFF;                 /* keep the bookkeeping honest  */
        return;
    }

    /*
     * TX teardown first (no-ops when not transmitting): dekey the AT1846S
     * carrier, then stop the C7000 FM-TX engine and drop back to the
     * RX-side work_mode.  Order mirrors the verified bring-up: carrier
     * off before the modulator so we don't radiate an unmodulated blip.
     */
    if(radioStatus == TX)
    {
        DAC_DATA_B   = 0u;                     // APC drive to zero first
        hd2_at1846s_write(0x30, 0x4046u);      // PA off (bit7 clear) first...
        hd2_at1846s_write(0x30, 0x4006u);      // ...then tx_on off (dekey)
        hd2_at1846s_write(0x0a, 0x4c20u);      // restore padrv to chip-init default
        DAC_PD_CTRL |= 0x2u;                   // APC DAC ch B power down
        gpio_atomic_clear(&GPIOB_DR, (1u << 3));   // mic gate off
        SOCSYS_FM_PTT     = 0u;
        SOCSYS_WORK_MODE &= ~WORK_MODE_FM_MOD;
        hd2_at1846s_write(0x40, 0x0031u);      // RX-side AF-DSP ctrl value
    }

    // Drop both RX and TX on the chip side and silence any tone output.
    at1846s.disableTone();
    at1846s.disableCtcss();
    at1846s.setFuncMode(AT1846S_FuncMode::OFF);
    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    if(g_rf_freeze != 0u)                  /* rf_freeze: no AT1846S writes */
        return;

    currRxBand = getBandFromFrequency(config->rxFrequency);

    if(currRxBand == BND_NONE)
        return;

    /*
     * Analog-FM bandwidth select.  No flash calibration backend yet
     * (#73), so the per-band noise/RSSI/squelch threshold tuning that
     * GDx pulls from calData is left at the AT1846S init defaults.  Once
     * an HD2 calibration source exists, mirror radio_GDx.cpp's
     * setNoise1/2Thresholds + setRssiThresholds + setAnalogSqlThresh.
     */
    if(config->opMode == OPMODE_FM)
    {
        switch(config->bandwidth)
        {
            case BW_12_5:
                at1846s.setBandwidth(AT1846S_BW::_12P5);
                break;

            case BW_25:
                at1846s.setBandwidth(AT1846S_BW::_25);
                break;

            default:
                break;
        }
    }

    /*
     * Re-apply the VCO frequency / RX path if we are already receiving,
     * so a config change (e.g. a new RX frequency) takes effect without
     * the core having to cycle the op-status.  Mirrors radio_GDx.cpp.
     */
    if(radioStatus == RX)
        radio_enableRx();
}

rssi_t radio_getRssi()
{
    /*
     * AT1846S reg 0x1B upper byte holds the RSSI level; the generic
     * driver maps it to dBm as (-137 + (reg >> 8)).  Vendor V2.1.3
     * FUN_03040994 reads the same register ((v & 0x7FFF) >> 8) for its
     * 5-level signal-bar mapping, confirming the field location.
     *
     * rf_freeze: even a *read* is bit-bang bus traffic that can corrupt a
     * concurrent host transaction, so return the last cached value while
     * frozen (the S-meter just holds still).
     */
    static rssi_t lastRssi = -127;

    if(g_rf_freeze == 0u)
        lastRssi = static_cast<rssi_t>(at1846s.readRSSI());

    return lastRssi;
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}
