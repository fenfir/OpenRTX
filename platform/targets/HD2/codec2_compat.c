/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * codec2_create / codec2_destroy bridge over the Codec2-mod fork (see codec2.h).
 * Compiled into libcodec2_hd2.a with -DC2_FIXED so sizeof(codec2_t) and
 * codec2_init() see the same fixed-point struct layout as the rest of the fork.
 * The fork keeps all working state inside the caller-owned struct (no internal
 * malloc), so a single heap allocation here is the whole footprint (~32 KB).
 */
#include "codec2.h"
#include <stdlib.h>

struct CODEC2 *codec2_create(int mode)
{
    (void)mode; /* fork is 3200-only */

    codec2_t *c2 = malloc(sizeof(codec2_t));
    if (c2 != NULL)
        codec2_init(c2);

    return c2;
}

void codec2_destroy(struct CODEC2 *c2)
{
    free(c2);
}
