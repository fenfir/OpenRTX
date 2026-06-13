/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_HDLC_H
#define APRS_HDLC_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "protocols/APRS/Constants.hpp"

namespace Aprs
{

/// A de-framed, FCS-validated frame payload (FCS bytes stripped).
struct Frame
{
    uint8_t data[MAX_FRAME];
    int     len;
};

/**
 * Layer 2 (data link): streaming HDLC de-framer for AX.25.
 *
 * NRZI-decodes the symbol stream, detects 0x7E flags, removes bit-stuffing,
 * assembles bytes between flags, and validates the CRC-16/X.25 FCS.  Feed one
 * demodulated symbol at a time.
 */
class HdlcDecoder
{
public:
    HdlcDecoder();

    /**
     * \brief Reset the de-framer state (does not clear diagnostics).
     */
    void reset();

    /**
     * \brief Feed one demodulated symbol.
     *
     * @param symbol: 1 = mark, 0 = space.
     * @param out: receives the frame payload when a valid frame closes.
     * @return true when a complete, FCS-valid frame closed this symbol.
     */
    bool process(int symbol, Frame& out);

    /// Number of HDLC flags seen (bring-up diagnostic).
    int flagsSeen() const { return flagsSeen_; }
    /// Largest frame assembled, in bytes (bring-up diagnostic).
    int maxFrameLen() const { return maxLen_; }
    /// Longest frame assembled between flags, FCS pass OR fail (bring-up
    /// diagnostic: lets a host diff a near-miss against the expected bytes).
    const uint8_t* lastRawFrame() const { return raw_; }
    int lastRawLen() const { return rawLen_; }

private:
    void addDataBit(int bit);
    bool endFrame(Frame& out);

    int      nrziLast_;
    uint32_t shift_;        ///< last 8 NRZI-decoded bits, for flag detection.
    bool     inFrame_;
    int      ones_;         ///< consecutive 1s, for de-stuffing.
    int      bitCnt_;
    uint8_t  cur_;          ///< current byte being assembled (LSB-first).
    uint8_t  frame_[MAX_FRAME];
    int      flen_;
    int      flagsSeen_;
    int      maxLen_;
    uint8_t  raw_[MAX_FRAME];   ///< longest assembled frame (diagnostic).
    int      rawLen_;
};

} // namespace Aprs

#endif // APRS_HDLC_H
