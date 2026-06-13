/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/Ax25.hpp"
#include <cstdio>
#include <cstring>

namespace Aprs
{

// Decode one 7-byte AX.25 address (shifted ASCII + SSID); returns the
// address-extension bit (1 = last address) and writes "CALL[-SSID]" to out.
static int decodeAddr(const uint8_t* a, char* out)
{
    int p = 0;
    for(int i = 0; i < 6; ++i)
    {
        char c = (char)(a[i] >> 1);
        if(c != ' ') out[p++] = c;
    }
    int ssid = (a[6] >> 1) & 0x0f;
    if(ssid)
    {
        out[p++] = '-';
        if(ssid >= 10) { out[p++] = '1'; ssid -= 10; }
        out[p++] = (char)('0' + ssid);
    }
    out[p] = 0;
    return a[6] & 1;
}

int formatAx25(const uint8_t* frame, int len, char* out, int outSize)
{
    if(len < 16) return 0;

    char dst[12], src[12], path[64];
    path[0] = 0;
    decodeAddr(frame, dst);
    int last = decodeAddr(frame + 7, src);

    int o = 14;
    while(!last && o + 7 <= len)
    {
        char dg[12];
        last = decodeAddr(frame + o, dg);
        o += 7;
        if((int)(strlen(path) + strlen(dg) + 2) < (int)sizeof(path))
        {
            strcat(path, ",");
            strcat(path, dg);
        }
    }

    if(o + 2 > len || frame[o] != 0x03) return 0;   // not a UI frame
    o += 2;                                          // skip control + PID

    int w = snprintf(out, outSize, "%s>%s%s:", src, dst, path);
    for(int i = o; i < len && w < outSize - 1; ++i)
    {
        char c = (char)frame[i];
        out[w++] = (c >= 32 && c < 127) ? c : '.';
    }
    out[(w < outSize) ? w : outSize - 1] = 0;
    return w;
}

} // namespace Aprs
