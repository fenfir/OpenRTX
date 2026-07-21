/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenRTX audio output-stream driver for the Ailunce HD2: CPU -> HR_C7000
 * codec DAC via the SAHB PCM mailbox (radio-free -- no AT1846S/RF).
 *
 * The "DMA" here is the SoC PCM-bridge frame IRQ (PIC source 0x1b): once the
 * bridge is armed (pcm_mode=3, voice_path bit0+0x20) the codec requests one
 * 80-sample frame every 10 ms; the ISR acks the handshake
 * (SOCSYS_INT_STATUS |= 0x20) and copies the next frame from the stream buffer
 * into the playback window at 0x180000a0. Mono s16, 8 kHz only.
 *
 * Single stream instance (one codec DAC).
 */

#include <interfaces/audio.h>
#include <interfaces/interrupts.h>
#include <miosix.h>
#include <errno.h>
#include <string.h>
#include "hd2_regs.h"

using namespace miosix;

namespace {

struct StreamState
{
    struct streamCtx *ctx;      // active stream context (NULL when idle)
    size_t            readPos;  // next sample index the ISR will consume
    Thread *volatile  waiting;  // thread blocked in sync(), if any
    volatile uint8_t  syncFlag; // a half/end boundary was crossed
    volatile uint8_t  stopReq;  // graceful stop: end at next boundary
    uint32_t          vpSave;   // voice_path value to restore at stop
};

StreamState st = {nullptr, 0, nullptr, 0, 0, 0};

} // namespace

namespace {

// Disarm the PCM bridge and end the stream. IRQ or locked context.
void streamHalt()
{
    SOCSYS_VOICE_PATH = st.vpSave; // disarming stops the frame IRQs
    if(st.ctx != nullptr)
        st.ctx->running = 0;

    st.ctx = nullptr;
    Thread *w = st.waiting;
    st.waiting = nullptr;
    if(w != nullptr)
        w->IRQwakeup();
}

// 100 Hz PCM frame IRQ: handshake first, then supply the next 80 samples.
void pcmStreamIsr(void *)
{
    SOCSYS_INT_STATUS |= INT_STATUS_PCM_PLAY_ACK;

    struct streamCtx *ctx = st.ctx;
    if((ctx == nullptr) || (ctx->running == 0))
        return;

    volatile uint16_t *dst = SAHB_PCM_PLAY;
    size_t pos = st.readPos;
    size_t n   = ctx->bufSize - pos;
    if(n > PCM_FRAME_SAMPLES)
        n = PCM_FRAME_SAMPLES;

    for(size_t i = 0; i < n; ++i)
        dst[i] = static_cast<uint16_t>(ctx->buffer[pos + i]);
    for(size_t i = n; i < PCM_FRAME_SAMPLES; ++i)
        dst[i] = 0; // zero-pad a ragged tail frame

    size_t oldPos = pos;
    pos += n;

    const size_t half = ctx->bufSize / 2;
    bool boundary = false;

    if(pos >= ctx->bufSize)
    {
        boundary = true;
        if(ctx->bufMode == BUF_CIRC_DOUBLE)
            pos = 0;
    }
    else if((oldPos < half) && (pos >= half))
    {
        boundary = true;
    }

    st.readPos = pos;

    if(boundary)
    {
        st.syncFlag = 1;

        bool ended = (ctx->bufMode != BUF_CIRC_DOUBLE) && (pos >= ctx->bufSize);
        if((st.stopReq != 0) || ended)
        {
            streamHalt();
            return;
        }

        Thread *w = st.waiting;
        st.waiting = nullptr;
        if(w != nullptr)
            w->IRQwakeup();
    }
}

} // namespace

static int hd2pcm_start(const uint8_t instance, const void *config,
                        struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if((ctx == NULL) || (ctx->running != 0))
        return -EINVAL;
    if((ctx->bufSize < PCM_FRAME_SAMPLES) || (ctx->buffer == NULL))
        return -EINVAL;
    if(ctx->sampleRate != 8000)
        return -EINVAL; // the PCM bridge is fixed at 8 kHz

    GlobalIrqLock lock;

    if(st.ctx != nullptr)
        return -EBUSY; // single codec DAC, single stream

    ctx->priv   = &st;
    st.ctx      = ctx;
    st.readPos  = 0;
    st.waiting  = nullptr;
    st.syncFlag = 0;
    st.stopReq  = 0;

    // Arm the PCM bridge on top of the (already-warm) codec DAC.
    st.vpSave = SOCSYS_VOICE_PATH;
    SOCSYS_SYS_SOFT_RSTN |= SOFT_RSTN_PCM_BITS;
    SOCSYS_PCM_MODE = 3u;
    SOCSYS_VOICE_PATH |= (VOICE_PATH_PCM_EN | VOICE_PATH_PLAY);

    IRQregisterIrq(lock, HD2_IRQ_PCM_PLAY, &pcmStreamIsr, nullptr);

    ctx->running = 1;

    // Kick the pump: supply frame zero + the frame-supplied handshake.
    pcmStreamIsr(nullptr);
    return 0;
}

static int hd2pcm_idleBuf(struct streamCtx *ctx, stream_sample_t **buf)
{
    if(ctx->bufMode != BUF_CIRC_DOUBLE)
    {
        *buf = NULL;
        return -1;
    }

    const size_t half = ctx->bufSize / 2;

    FastGlobalIrqLock lock;
    if(st.readPos < half)
        *buf = ctx->buffer + half; // ISR consuming first half
    else
        *buf = ctx->buffer; // ISR consuming second half

    return (int)half;
}

static int hd2pcm_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    FastGlobalIrqLock lock;

    if(ctx->running == 0)
        return -1;
    if(st.waiting != nullptr)
        return -1; // another thread already at syncpoint

    while((st.syncFlag == 0) && (ctx->running != 0))
    {
        st.waiting = Thread::getCurrentThread();
        Thread::IRQglobalIrqUnlockAndWait(lock);
    }

    st.syncFlag = 0;
    st.waiting  = nullptr;
    return 0;
}

static void hd2pcm_stop(struct streamCtx *ctx)
{
    FastGlobalIrqLock lock;
    if((ctx->running == 0) || (st.ctx != ctx))
        return;

    st.stopReq = 1; // ISR halts at the next boundary
}

static void hd2pcm_halt(struct streamCtx *ctx)
{
    FastGlobalIrqLock lock;
    if((ctx->running == 0) || (st.ctx != ctx))
        return;

    streamHalt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern "C" const struct audioDriver hd2_pcm_audio_driver = {
    .start     = hd2pcm_start,
    .data      = hd2pcm_idleBuf,
    .sync      = hd2pcm_sync,
    .stop      = hd2pcm_stop,
    .terminate = hd2pcm_halt};
#pragma GCC diagnostic pop
