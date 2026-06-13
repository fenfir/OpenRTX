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

} // namespace Aprs

#endif // APRS_AX25_H
