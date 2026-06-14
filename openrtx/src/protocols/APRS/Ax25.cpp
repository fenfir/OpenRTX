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

uint16_t crc16X25(const uint8_t* data, int len)
{
    uint16_t crc = 0xFFFF;
    for(int i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for(int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    }
    return crc ^ 0xFFFF;
}

// Encode one AX.25 address field: 6 chars shifted left 1, space-padded, then
// the SSID byte (0x60 | ssid<<1 | last-bit).
static int encodeAddr(const char* call, int ssid, int last, uint8_t* out)
{
    int i = 0;
    for(; i < 6 && call[i]; ++i) out[i] = (uint8_t)(call[i] << 1);
    for(; i < 6; ++i)            out[i] = (uint8_t)(' ' << 1);
    out[6] = (uint8_t)(0x60 | ((ssid & 0x0f) << 1) | (last & 1));
    return 7;
}

int buildUi(const char* dst, int dstSsid, const char* src, int srcSsid,
            const char* digi, int digiSsid, const char* info,
            uint8_t* out, int outSize)
{
    int need = 14 + (digi ? 7 : 0) + 2;
    int infolen = 0; while(info[infolen]) infolen++;
    if(need + infolen > outSize) return 0;

    int o = 0;
    o += encodeAddr(dst, dstSsid, 0, out + o);
    o += encodeAddr(src, srcSsid, digi ? 0 : 1, out + o);
    if(digi) o += encodeAddr(digi, digiSsid, 1, out + o);
    out[o++] = 0x03;            // UI control
    out[o++] = 0xF0;            // PID: no layer 3
    for(int i = 0; i < infolen; ++i) out[o++] = (uint8_t)info[i];
    return o;
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
