/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone HR_C7000 codec audio-output warm-up (radio-free) for the HD2.
 */

#ifndef CODEC_HD2_H
#define CODEC_HD2_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the codec DAC -> LINE2OUT audio-output path once (idempotent).
 * Must run before any beep/PCM playback is audible. No RF / AT1846S. */
void hd2_audio_out_warm(void);

/* Gate the codec DAC lineout: on before a prompt, off when idle (silent state)
 * so the warm DAC doesn't leak a faint idle hiss past the muted amp. */
void hd2_audio_out_lineout(int on);

#ifdef __cplusplus
}
#endif

#endif /* CODEC_HD2_H */
