/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_AFSK1200_H
#define APRS_AFSK1200_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "protocols/APRS/Constants.hpp"

namespace Aprs
{

/**
 * Layer 1 (physical): 1200-baud Bell-202 AFSK demodulator.
 *
 * Integer-only and O(1) memory: a non-coherent mark/space correlator decides
 * the instantaneous symbol per sample, and a bit-timing PLL samples it at the
 * bit centres.  Streaming -- feed one 8 kHz audio sample at a time.
 */
class Afsk1200Demodulator
{
public:
    Afsk1200Demodulator();

    /**
     * \brief Reset the demodulator state.
     */
    void reset();

    /**
     * \brief Process one demod-audio sample.
     *
     * @param sample: signed 8 kHz audio sample.
     * @param symbol: receives the recovered symbol (1 = mark, 0 = space) when
     *                the bit-timing PLL fires.
     * @return true when a symbol was recovered this sample, false otherwise.
     */
    bool process(int16_t sample, int& symbol);

private:
    int32_t dcAcc_;             ///< DC-blocker accumulator (>>10 IIR).
    int16_t ring_[CORR_TAPS];   ///< last CORR_TAPS DC-removed samples.
    int     rpos_;
    int     phase_;             ///< bit-timing PLL phase (256ths of a sample).
    int     lastSym_;           ///< previous symbol decision (edge sync).
};

} // namespace Aprs

#endif // APRS_AFSK1200_H
