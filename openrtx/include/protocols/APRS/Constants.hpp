/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_CONSTANTS_H
#define APRS_CONSTANTS_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

namespace Aprs
{

/// 1200-baud Bell-202 AFSK parameters, sampled at the radio's audio rate.
constexpr int SAMPLE_RATE = 8000;   ///< Demod-audio sample rate (Hz).
constexpr int BAUD        = 1200;   ///< Symbol rate (symbols/s).
constexpr int MARK_FREQ   = 1200;   ///< Mark tone (Hz).
constexpr int SPACE_FREQ  = 2200;   ///< Space tone (Hz).

constexpr int CORR_TAPS   = 7;      ///< Correlator window ~= one bit (round FS/BAUD).
constexpr int SPB_Q8      = (SAMPLE_RATE * 256) / BAUD;  ///< samples/bit << 8.

constexpr int MAX_FRAME   = 340;    ///< Max assembled AX.25 frame length (bytes).

} // namespace Aprs

#endif // APRS_CONSTANTS_H
