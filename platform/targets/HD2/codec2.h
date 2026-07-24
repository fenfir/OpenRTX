/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shim mapping OpenRTX's expected upstream codec2 API onto the M17-Project
 * "Codec2-mod" fork (vendor/Codec2-mod), a 3200-only, malloc-free, fixed-point
 * codec2 for the no-FPU CK803S.  OpenRTX core (audio_codec.c) refers to the
 * handle as `struct CODEC2` and calls codec2_create / codec2_destroy /
 * codec2_encode / codec2_decode; the fork instead exposes codec2_init /
 * codec2_encode / codec2_decode on a caller-owned `struct codec2_t`.
 *
 * The fork's per-call encode/decode names and argument order already match
 * upstream, so we only:
 *   - alias `struct CODEC2` to the fork's `struct codec2_t` (so the fork's
 *     codec2_encode/codec2_decode satisfy the core's calls unchanged), and
 *   - add codec2_create / codec2_destroy (allocate + codec2_init / free) plus
 *     the CODEC2_MODE_3200 constant the core passes.
 *
 * audio_codec.c uses `struct CODEC2` only through a pointer, so it need not be
 * built with -DC2_FIXED; codec2_compat.c (which sizeof()s and initialises the
 * struct) lives inside libcodec2_hd2.a and is built with the same -DC2_FIXED as
 * the rest of the fork, keeping the struct layout consistent.
 */
#ifndef HD2_CODEC2_SHIM_H
#define HD2_CODEC2_SHIM_H

#include "codec2_mod.h" /* struct codec2_t (complete), codec2_init/encode/decode */

/* OpenRTX names the handle `struct CODEC2`; make that tag the fork's struct. */
#define CODEC2 codec2_t

/* The fork is 3200-only; the mode argument is accepted and ignored. */
enum
{
    CODEC2_MODE_3200 = 0
};

#ifdef __cplusplus
extern "C" {
#endif

struct CODEC2 *codec2_create(int mode);
void codec2_destroy(struct CODEC2 *c2);

#ifdef __cplusplus
}
#endif

#endif /* HD2_CODEC2_SHIM_H */
