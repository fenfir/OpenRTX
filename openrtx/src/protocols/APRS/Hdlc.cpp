/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/Hdlc.hpp"

namespace Aprs
{

static uint16_t crc16_x25(const uint8_t* d, int n)
{
    uint16_t crc = 0xFFFF;
    for(int i = 0; i < n; ++i)
    {
        crc ^= d[i];
        for(int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    }
    return crc ^ 0xFFFF;
}

HdlcDecoder::HdlcDecoder()
{
    reset();
    flagsSeen_ = 0;
    maxLen_    = 0;
}

void HdlcDecoder::reset()
{
    nrziLast_ = 1;
    shift_    = 0;
    inFrame_  = false;
    ones_     = 0;
    bitCnt_   = 0;
    cur_      = 0;
    flen_     = 0;
}

void HdlcDecoder::addDataBit(int bit)
{
    cur_ |= (uint8_t)((bit & 1) << bitCnt_);
    if(++bitCnt_ == 8)
    {
        if(flen_ < MAX_FRAME) frame_[flen_++] = cur_;
        if(flen_ > maxLen_)   maxLen_ = flen_;
        cur_    = 0;
        bitCnt_ = 0;
    }
}

bool HdlcDecoder::endFrame(Frame& out)
{
    bool got = false;
    if(flen_ >= 4)
    {
        int n = flen_ - 2;
        uint16_t rx = (uint16_t)frame_[n] | ((uint16_t)frame_[n + 1] << 8);
        if(crc16_x25(frame_, n) == rx)
        {
            for(int i = 0; i < n; ++i) out.data[i] = frame_[i];
            out.len = n;
            got = true;
        }
    }
    inFrame_ = false;
    flen_    = 0;
    bitCnt_  = 0;
    cur_     = 0;
    ones_    = 0;
    return got;
}

bool HdlcDecoder::process(int symbol, Frame& out)
{
    // NRZI decode: no change -> 1, change -> 0.
    int dbit = (symbol == nrziLast_) ? 1 : 0;
    nrziLast_ = symbol;

    // Track the last 8 NRZI-decoded bits to spot the 0x7E flag.
    shift_ = ((shift_ << 1) | (uint32_t)dbit) & 0xff;

    if(shift_ == 0x7e)          // HDLC flag
    {
        flagsSeen_++;
        bool done = inFrame_ ? endFrame(out) : false;
        inFrame_ = true;
        flen_ = 0; bitCnt_ = 0; cur_ = 0; ones_ = 0;
        return done;
    }
    if(!inFrame_) return false;

    if(dbit == 1)
    {
        ones_++;
        if(ones_ >= 7) { inFrame_ = false; ones_ = 0; return false; }  // abort
        addDataBit(1);          // a flag's 6 ones land in a partial byte,
    }                           // which endFrame() discards.
    else
    {
        if(ones_ == 5) { ones_ = 0; return false; }  // drop the stuffed 0
        ones_ = 0;
        addDataBit(0);
    }
    return false;
}

} // namespace Aprs
