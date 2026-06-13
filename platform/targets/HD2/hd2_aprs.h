/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Streaming 1200-baud AFSK + HDLC + AX.25 decoder (see hd2_aprs.c).
 */
#ifndef HD2_APRS_H
#define HD2_APRS_H
#include <stdint.h>

#define APRS_NTAP   7
#define APRS_MAXFR  340

typedef struct
{
    /* DC blocker (running mean, >>10 IIR) */
    int32_t  dc;
    /* correlator ring of the last NTAP DC-removed samples */
    int16_t  ring[APRS_NTAP];
    int      rpos;
    /* bit-timing PLL (256ths of a sample) */
    int      phase;
    int      lastsym;             /* last mark/space decision (for edge sync)  */
    /* NRZI */
    int      nrzi_last;
    int      nrzi_init;
    /* HDLC bit assembler */
    uint32_t shift;               /* last 8 raw NRZI-decoded bits (for flag)   */
    int      inframe;
    int      ones;                /* consecutive 1s (for de-stuff)             */
    int      bitcnt;              /* bits into current byte                    */
    uint8_t  cur;                 /* current byte being assembled (LSB-first)  */
    uint8_t  frame[APRS_MAXFR];
    int      flen;
    /* diagnostics (HW bring-up): HDLC flags seen, largest frame assembled */
    int      dbg_flags;
    int      dbg_maxflen;
} aprs_t;

#ifdef __cplusplus
extern "C" {
#endif
void aprs_init(aprs_t *a);
int  aprs_feed(aprs_t *a, const int16_t *s, int n, char *out, int outsz);
#ifdef __cplusplus
}
#endif
#endif
