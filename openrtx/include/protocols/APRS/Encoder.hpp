/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_ENCODER_H
#define APRS_ENCODER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "protocols/APRS/Constants.hpp"

namespace Aprs
{

/// Max NRZI symbols (preamble flags + frame + FCS + stuffing + trailing flags).
constexpr int MAX_SYMBOLS = 1200;

/**
 * APRS transmit encoder: builds an AX.25 UI beacon, HDLC-frames it (FCS,
 * bit-stuffing, 0x7E flags), NRZI-encodes it, and acts as a streaming
 * continuous-phase Bell-202 AFSK modulator -- pull one 8 kHz sample at a time
 * (no large buffer; feed straight into the radio's TX PCM path).
 *
 * Symmetric with Decoder; integer-only NCO.
 */
class Encoder
{
public:
    Encoder();

    /**
     * \brief Build an APRS UI-frame beacon and arm the modulator.
     *
     * @param dst,dstSsid: destination (e.g. "APRS"/"APZHD2").
     * @param src,srcSsid: source callsign + SSID.
     * @param digi,digiSsid: one digipeater (e.g. "WIDE1"/1), or NULL for none.
     * @param info: APRS information field text.
     * @param preambleFlags: number of leading 0x7E flags (TX warm-up).
     */
    void buildBeacon(const char* dst, int dstSsid, const char* src, int srcSsid,
                     const char* digi, int digiSsid, const char* info,
                     int preambleFlags);

    /// True while there are samples left to transmit.
    bool busy() const { return symIdx_ < nsym_; }

    /**
     * \brief Yield the next 8 kHz int16 AFSK sample.
     * @param out: receives the sample.
     * @return false when the whole frame has been emitted.
     */
    bool nextSample(int16_t& out);

    /// Number of NRZI symbols in the built frame (for tone-generator TX, where
    /// the modulator is driven one symbol at a time rather than per PCM sample).
    int numSymbols() const { return nsym_; }

    /// NRZI symbol \p i: true = mark (1200 Hz), false = space (2200 Hz).
    bool symbolIsMark(int i) const { return (i >= 0 && i < nsym_) && (sym_[i] != 0); }

private:
    uint8_t  sym_[MAX_SYMBOLS];  ///< NRZI symbol levels (1 = mark, 0 = space).
    int      nsym_;
    int      symIdx_;
    int      sampAccum_;         ///< bit-timing accumulator (256ths of a sample).
    uint32_t phase_;             ///< NCO phase accumulator (continuous-phase FSK).
};

} // namespace Aprs

#endif // APRS_ENCODER_H
