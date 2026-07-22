/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HD2 codec (core/audio_codec.h) -- STREAMING voice-prompt decode.
 *
 * A background worker opens a BUF_CIRC_DOUBLE output stream (two 160-sample
 * halves) on the HR_C7000 codec DAC and, for each half boundary, pops one raw
 * codec2 3200 frame from a queue, decodes it straight into the idle half, and
 * syncs -- the stock OpenRTX streaming model. pushFrame() enqueues raw 8-byte
 * frames (~0.6 KB for a whole prompt) instead of pre-decoding to a 25 KB PCM
 * buffer, so playback starts after the first frame (~1 slot) rather than after
 * the whole prompt decodes. Real-time-tight: each frame must decode within its
 * 20 ms play slot or the ring underruns (audible glitch) -- viable only now that
 * decode is near the budget.
 *
 * A one-shot boot warmup (codec_init) pays the ~475 ms first-decode cost (prune
 * schedule + decode-code first-touch) behind the splash, so the first prompt is
 * warm. codec_running() stays true across the whole stream so voicePrompts.c
 * holds the audio path until playback actually finishes.
 *
 * Encode (mic->codec2) is not provided here (radio-free min).
 */

#include "core/audio_codec.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/audio.h"
#include <pthread.h>
#include <string.h>
#include <errno.h>
/* codec2 header LAST: codec2_internal.h #defines short names (T, DEC, ...) that
 * collide with system/template identifiers if seen first. */
#include "codec2_mod.h"

#define VP_NSAMP     160u /* CODEC2_SAMPLES_PER_FRAME (3200) = 20 ms */
#define Q_CAP        96u  /* raw 8-byte frames buffered (holds a whole prompt) */
#define HALF_FRAMES  4u   /* frames per double-buffer half: batch gives the decoder
                           * slack to absorb per-frame spikes (80 ms of runway) */

static codec2_t c2;

/* Raw codec2-frame queue: producer = codec_pushFrame, consumer = streamWorker. */
static uint8_t         q[Q_CAP][8];
static unsigned        qHead, qTail, qCount;
static pthread_mutex_t qLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  qCond = PTHREAD_COND_INITIALIZER;

static volatile bool codecReady = false; /* boot warmup finished              */
static volatile bool running    = false; /* worker active (decode+play)       */
static volatile bool finishing  = false; /* prompt fully pushed: drain + stop */
static volatile bool stopReq    = false; /* hard abort (PTT etc.)             */
static pathId        vpPath     = -1;

/* ---- boot warmup -------------------------------------------------------- */
/* A real codec2 3200 frame ("zero" prompt frame 0), decoded once at boot. */
static const uint8_t warmupFrame[8] = {192, 0, 106, 67, 156, 228, 33, 8};

static void *warmupWorker(void *arg)
{
    (void)arg;
    static int16_t scratch[VP_NSAMP];
    codec2_decode(&c2, scratch, warmupFrame);
    codec2_init(&c2);   /* reset IIR state; persistent FFT/prune tables stay warm */
    codecReady = true;
    return NULL;
}

void codec_init()
{
    static bool started = false;
    if(started)
        return;
    started = true;
    codec2_init(&c2);

    /* Warm the decoder off the boot path (see file header). codec2_decode
     * overflows the 2 KB default pthread stack, so use 16 KB. */
    pthread_t w;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);
    if(pthread_create(&w, &attr, &warmupWorker, NULL) == 0)
        pthread_detach(w);
    else
        codecReady = true; /* couldn't spawn: first prompt runs cold */
    pthread_attr_destroy(&attr);
}

/* Pop one raw frame from the queue (blocking until one arrives, the prompt is
 * finished, or an abort). Returns true if a frame was written to `frame`. */
static bool stream_pop(uint8_t *frame)
{
    bool have = false;
    pthread_mutex_lock(&qLock);
    while((qCount == 0) && !finishing && !stopReq)
        pthread_cond_wait(&qCond, &qLock);
    if(qCount > 0)
    {
        memcpy(frame, q[qTail], 8);
        qTail = (qTail + 1) % Q_CAP;
        qCount--;
        have = true;
    }
    pthread_mutex_unlock(&qLock);
    return have;
}

/* Decode `n` frames into `dst`; zero-pad and set *drained once the prompt ends. */
static void stream_fill(int16_t *dst, unsigned n, bool *drained)
{
    for(unsigned k = 0; k < n; ++k)
    {
        uint8_t frame[8];
        if(!*drained && !stopReq && stream_pop(frame))
            codec2_decode(&c2, dst + k * VP_NSAMP, frame);
        else
        {
            memset(dst + k * VP_NSAMP, 0, VP_NSAMP * sizeof(int16_t));
            *drained = true;
        }
    }
}

/* ---- streaming decode worker ------------------------------------------- */
static void *streamWorker(void *arg)
{
    (void)arg;

    /* Wait until the whole prompt's raw frames are queued (finishing) before any
     * playback. Raw codec2 frames are 8 B each, so buffering an entire prompt is
     * cheap (Q_CAP holds it), and it guarantees the decoder never starves
     * mid-stream -- starvation makes the double-buffer replay a stale half (heard
     * as the prompt's start repeating). */
    pthread_mutex_lock(&qLock);
    while(!finishing && !stopReq && qCount < Q_CAP)
        pthread_cond_wait(&qCond, &qLock);
    pthread_mutex_unlock(&qLock);

    /* Warm the codec DAC + route/unmute the speaker amp (PTB17 LOW / PTB4 LOW). */
    audio_connect(SOURCE_MCU, SINK_SPK);

    static int16_t dbuf[2 * HALF_FRAMES * VP_NSAMP];
    bool drained = false;

    /* Prebuffer: decode BOTH halves before arming, so playback starts with a full
     * lead. Decode (~20.5 ms) barely exceeds the 20 ms play slot, so this lead
     * absorbs the accumulated per-frame drift over a whole prompt (covers ~100+
     * frames), eliminating the mid-prompt underrun "fan". Without it the ISR races
     * the decoder from frame 0 and drifts straight into underrun. */
    stream_fill(dbuf, 2 * HALF_FRAMES, &drained);

    streamId sid = audioStream_start(vpPath, dbuf, 2 * HALF_FRAMES * VP_NSAMP, 8000,
                                     STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if(sid < 0)
    {
        audio_disconnect(SOURCE_MCU, SINK_SPK); /* re-mute: couldn't arm the stream */
        running = false;
        return NULL;
    }

    /* Refill each half as the ISR frees it. sync(false) waits for the first half
     * to be consumed before getIdleBuffer hands it back, so we never overwrite the
     * pre-filled half still playing. */
    while(!stopReq && !drained)
    {
        outputStream_sync(sid, false); /* wait for a boundary (a half consumed) */
        int16_t *half = outputStream_getIdleBuffer(sid);
        if(half == NULL)
            break;
        stream_fill(half, HALF_FRAMES, &drained);
    }

    outputStream_sync(sid, false); /* let the last filled half play out */
    audioStream_stop(sid);
    /* Silent state: re-mute the speaker amp so nothing hisses between prompts.
     * Symmetric with the audio_connect() above; the codec-DAC PCM bridge is
     * already disarmed by audioStream_stop. */
    audio_disconnect(SOURCE_MCU, SINK_SPK);
    running = false;
    return NULL;
}

bool codec_startEncode(const pathId path)
{
    (void)path;
    return false; /* no mic/encode path in the radio-free min */
}

bool codec_startDecode(const pathId path)
{
    if(!codecReady || running)
        return false;

    vpPath    = path;
    qHead = qTail = qCount = 0;
    finishing = false;
    stopReq   = false;
    running   = true;

    pthread_t w;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384); /* codec2_decode > 2 KB default stack */
    if(pthread_create(&w, &attr, &streamWorker, NULL) == 0)
        pthread_detach(w);
    else
        running = false; /* spawn failed: report not-running so VP releases the path */
    pthread_attr_destroy(&attr);
    return running;
}

int codec_pushFrame(const uint8_t *frame, const bool blocking)
{
    (void)blocking;
    if(!running)
        return -EPERM;

    pthread_mutex_lock(&qLock);
    if(qCount >= Q_CAP) /* full: drop the oldest so the producer never stalls */
    {
        qTail = (qTail + 1) % Q_CAP;
        qCount--;
    }
    memcpy(q[qHead], frame, 8);
    qHead = (qHead + 1) % Q_CAP;
    qCount++;
    pthread_cond_signal(&qCond);
    pthread_mutex_unlock(&qLock);
    return 0;
}

int codec_popFrame(uint8_t *frame, const bool blocking)
{
    (void)frame;
    (void)blocking;
    return -EPERM; /* no encode path */
}

void codec_stop(const pathId path)
{
    (void)path;
    /* voicePrompts.c calls this every tick once the prompt is fully pushed:
     * request a graceful drain (play the queued frames, then stop). Idempotent. */
    pthread_mutex_lock(&qLock);
    finishing = true;
    pthread_cond_signal(&qCond);
    pthread_mutex_unlock(&qLock);
}

/* min-specific: hard-abort an in-progress prompt (used by vp_stop on PTT etc.). */
void codec_abort()
{
    pthread_mutex_lock(&qLock);
    stopReq = true;
    pthread_cond_signal(&qCond);
    pthread_mutex_unlock(&qLock);
}

void codec_terminate()
{
    codec_abort();
}

bool codec_running()
{
    return running;
}
