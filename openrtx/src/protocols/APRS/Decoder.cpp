/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/Decoder.hpp"
#include "protocols/APRS/Ax25.hpp"

namespace Aprs
{

void Decoder::reset()
{
    demod_.reset();
    hdlc_.reset();
}

bool Decoder::process(const int16_t* samples, unsigned count, char* out, int outSize)
{
    Frame frame;
    for(unsigned i = 0; i < count; ++i)
    {
        int symbol;
        if(!demod_.process(samples[i], symbol)) continue;
        if(hdlc_.process(symbol, frame))
        {
            if(formatAx25(frame.data, frame.len, out, outSize) > 0)
                return true;
        }
    }
    return false;
}

} // namespace Aprs
