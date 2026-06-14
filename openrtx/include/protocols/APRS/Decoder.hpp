/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_DECODER_H
#define APRS_DECODER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "protocols/APRS/Afsk1200.hpp"
#include "protocols/APRS/Hdlc.hpp"

namespace Aprs
{

/**
 * Top-level APRS receiver: wires the AFSK demodulator (PHY) -> HDLC de-framer
 * (data link) -> AX.25 parse (layer 3).  Feed 8 kHz demod-audio samples; on a
 * complete, FCS-valid UI frame it formats "SRC>DEST[,path]:info".
 *
 * Integer-only, O(1) memory, ~400 B of state -- safe to hold as a static (the
 * HD2's Miosix thread stacks are small).
 */
class Decoder
{
public:
    Decoder() = default;

    /**
     * \brief Reset the whole pipeline.
     */
    void reset();

    /**
     * \brief Feed a block of demod-audio samples.
     *
     * @param samples: signed 8 kHz audio samples.
     * @param count: number of samples.
     * @param out: receives the decoded "SRC>DEST:info" string on success.
     * @param outSize: size of out.
     * @return true when a valid frame was decoded within this block.
     */
    bool process(const int16_t* samples, unsigned count, char* out, int outSize);

    /// HDLC flags seen so far (bring-up diagnostic).
    int flagsSeen() const { return hdlc_.flagsSeen(); }
    /// Largest frame assembled, in bytes (bring-up diagnostic).
    int maxFrameLen() const { return hdlc_.maxFrameLen(); }
    /// Current in-progress frame length, in bytes (capture-trigger diagnostic).
    int curFrameLen() const { return hdlc_.curFrameLen(); }
    /// Longest assembled frame's raw bytes (bring-up diagnostic).
    const uint8_t* lastRawFrame() const { return hdlc_.lastRawFrame(); }
    int lastRawLen() const { return hdlc_.lastRawLen(); }

private:
    Afsk1200Demodulator demod_;
    HdlcDecoder         hdlc_;
};

} // namespace Aprs

#endif // APRS_DECODER_H
