/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/Encoder.hpp"
#include "protocols/APRS/Ax25.hpp"
#include <cmath>

namespace Aprs
{

// 256-entry signed sine table (amplitude ~10000), built once.
static int16_t sineTab[256];
static bool    sineReady = false;

static void initSine()
{
    if(sineReady) return;
    for(int i = 0; i < 256; ++i)
        sineTab[i] = (int16_t)(10000.0 * std::sin(6.2831853 * i / 256.0));
    sineReady = true;
}

// Continuous-phase FSK phase increments (freq * 2^32 / 8000), precomputed.
static const uint32_t INC_MARK  = (uint32_t)(((uint64_t)MARK_FREQ  << 32) / SAMPLE_RATE);
static const uint32_t INC_SPACE = (uint32_t)(((uint64_t)SPACE_FREQ << 32) / SAMPLE_RATE);

Encoder::Encoder()
{
    initSine();
    nsym_ = 0; symIdx_ = 0; sampAccum_ = 0; phase_ = 0;
}

void Encoder::buildBeacon(const char* dst, int dstSsid, const char* src, int srcSsid,
                          const char* digi, int digiSsid, const char* info,
                          int preambleFlags)
{
    uint8_t frame[400];
    int flen = buildUi(dst, dstSsid, src, srcSsid, digi, digiSsid, info,
                       frame, (int)sizeof(frame));
    nsym_ = 0; symIdx_ = 0; sampAccum_ = 0; phase_ = 0;
    if(flen == 0) return;

    uint16_t fcs = crc16X25(frame, flen);
    frame[flen++] = (uint8_t)(fcs & 0xff);
    frame[flen++] = (uint8_t)((fcs >> 8) & 0xff);

    // NRZI-encode raw HDLC bits into sym_: bit 0 toggles the level, bit 1 holds.
    int level = 1, ones = 0;
    auto pushRaw = [&](int bit)
    {
        if(bit == 0) level ^= 1;
        if(nsym_ < MAX_SYMBOLS) sym_[nsym_++] = (uint8_t)level;
    };
    auto flag = [&]()
    {
        static const int f[8] = { 0, 1, 1, 1, 1, 1, 1, 0 };
        for(int i = 0; i < 8; ++i) pushRaw(f[i]);
    };

    for(int i = 0; i < preambleFlags; ++i) flag();      // TX warm-up flags
    for(int b = 0; b < flen; ++b)                        // data + FCS, LSB-first
    {
        for(int i = 0; i < 8; ++i)
        {
            int bit = (frame[b] >> i) & 1;
            pushRaw(bit);
            if(bit) { if(++ones == 5) { pushRaw(0); ones = 0; } }  // bit-stuff
            else    ones = 0;
        }
    }
    for(int i = 0; i < 3; ++i) flag();                   // trailing flags
}

bool Encoder::nextSample(int16_t& out)
{
    if(symIdx_ >= nsym_) return false;

    out = sineTab[(phase_ >> 24) & 0xff];
    phase_ += sym_[symIdx_] ? INC_MARK : INC_SPACE;

    sampAccum_ += 256;
    if(sampAccum_ >= SPB_Q8)
    {
        sampAccum_ -= SPB_Q8;
        symIdx_++;
    }
    return true;
}

} // namespace Aprs
