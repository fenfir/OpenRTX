/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_AX25_H
#define APRS_AX25_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace Aprs
{

/**
 * \brief Format an AX.25 UI frame as human-readable "SRC>DEST[,path]:info".
 *
 * Layer 3: decodes the AX.25 address fields (shifted ASCII + SSID), the
 * digipeater path, and the UI information field.
 *
 * @param frame: the de-framed AX.25 frame (FCS already stripped).
 * @param len: frame length in bytes.
 * @param out: destination buffer for the formatted, NUL-terminated string.
 * @param outSize: size of out.
 * @return number of characters written (0 if not a parseable UI frame).
 */
int formatAx25(const uint8_t* frame, int len, char* out, int outSize);

/**
 * \brief CRC-16/X.25 (the AX.25 frame check sequence).
 */
uint16_t crc16X25(const uint8_t* data, int len);

/**
 * \brief Build an AX.25 UI frame (no FCS; the HDLC layer appends it).
 *
 * Encodes dest + src (+ optional one digipeater) as shifted-ASCII/SSID address
 * fields, then the UI control (0x03), PID (0xF0) and the info text.
 *
 * @param dst,dstSsid: destination call + SSID.
 * @param src,srcSsid: source call + SSID.
 * @param digi: optional digipeater call (NULL for none).
 * @param digiSsid: digipeater SSID.
 * @param info: information field text.
 * @param out: destination buffer.
 * @param outSize: size of out.
 * @return frame length in bytes (0 on overflow).
 */
int buildUi(const char* dst, int dstSsid, const char* src, int srcSsid,
            const char* digi, int digiSsid, const char* info,
            uint8_t* out, int outSize);

} // namespace Aprs

#endif // APRS_AX25_H
