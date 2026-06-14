/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/Afsk1200.hpp"
#include <cstring>

namespace Aprs
{

/*
 * Integer tone-reference tables (cos/sin * 64) one bit-window long, for the
 * mark (1200 Hz) and space (2200 Hz) tones at 8 kHz.
 */
static const int cMark[CORR_TAPS] = {   64,   38,  -20,  -61,  -52,    0,   52 };
static const int sMark[CORR_TAPS] = {    0,   52,   61,   20,  -38,  -64,  -38 };
static const int cSpace[CORR_TAPS] = {  64,  -10,  -61,   29,   52,  -45,  -38 };
static const int sSpace[CORR_TAPS] = {   0,   63,  -20,  -57,   38,   45,  -52 };

Afsk1200Demodulator::Afsk1200Demodulator()
{
    reset();
}

void Afsk1200Demodulator::reset()
{
    dcAcc_ = 0;
    std::memset(ring_, 0, sizeof(ring_));
    rpos_    = 0;
    phase_   = 0;
    lastSym_ = 1;
}

bool Afsk1200Demodulator::process(int16_t sample, int& symbol)
{
    // DC blocker (running mean, >>10 IIR).
    dcAcc_ += (((int32_t)sample << 10) - dcAcc_) >> 10;
    int16_t x = (int16_t)(sample - (dcAcc_ >> 10));

    ring_[rpos_] = x;
    rpos_ = (rpos_ + 1) % CORR_TAPS;

    // Non-coherent correlator: mark vs space magnitude over the bit window.
    int32_t im = 0, qm = 0, is = 0, qs = 0;
    int idx = rpos_;
    for(int i = 0; i < CORR_TAPS; ++i)
    {
        int v = ring_[idx];
        im += v * cMark[i];  qm += v * sMark[i];
        is += v * cSpace[i]; qs += v * sSpace[i];
        idx = (idx + 1) % CORR_TAPS;
    }
    int64_t magMark  = (int64_t)im * im + (int64_t)qm * qm;
    int64_t magSpace = (int64_t)is * is + (int64_t)qs * qs;
    int sym = (magMark > magSpace) ? 1 : 0;

    // Bit-timing PLL: sample at the bit centre, re-centre on transitions.
    if(sym != lastSym_) phase_ = SPB_Q8 / 2;
    lastSym_ = sym;
    phase_ += 256;
    if(phase_ >= SPB_Q8)
    {
        phase_ -= SPB_Q8;
        symbol = sym;
        return true;
    }
    return false;
}

} // namespace Aprs
