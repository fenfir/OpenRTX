/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Streaming 1200-baud AFSK (Bell 202) + NRZI + HDLC + AX.25 decoder for the
 * Ailunce HD2.  Integer-only (the CK803S is soft-float) and O(1) memory -- it
 * processes the 8 kHz demod-audio sample stream incrementally (frame by frame
 * from the codec capture buffer), so there is NO large packet buffer (the
 * 16 KB whole-packet buffer starved the Miosix heap and hung boot).
 *
 * Algorithm mirrors the validated host reference (scripts/aprs_afsk.py).
 * Portable: compile with -DAPRS_HOST_TEST for a host self-test harness.
 *
 * On a complete, FCS-valid AX.25 UI frame, aprs_feed() formats the decoded
 * "SRC>DEST[,path]:info" line into the caller's buffer and returns 1.
 */
#include "hd2_aprs.h"
#include <string.h>
#include <stdio.h>

#define APRS_FS     8000
#define APRS_BAUD   1200
#define APRS_MARK   1200
#define APRS_SPACE  2200
#define APRS_SPB256 ((APRS_FS * 256) / APRS_BAUD)   /* samples/bit << 8 = 1706 */
/* APRS_NTAP, APRS_MAXFR, aprs_t come from hd2_aprs.h */

/* Integer tone reference tables (cos/sin * 64), one bit-window long. */
static const int cmark[APRS_NTAP] = {   64,   38,  -20,  -61,  -52,    0,   52 };
static const int smark[APRS_NTAP] = {    0,   52,   61,   20,  -38,  -64,  -38 };
static const int cspc [APRS_NTAP] = {   64,  -10,  -61,   29,   52,  -45,  -38 };
static const int sspc [APRS_NTAP] = {    0,   63,  -20,  -57,   38,   45,  -52 };

void aprs_init(aprs_t *a)
{
    memset(a, 0, sizeof(*a));
    a->lastsym  = 1;
    a->nrzi_last = 1;
}

static uint16_t crc16_x25(const uint8_t *d, int n)
{
    uint16_t crc = 0xFFFF;
    for(int i = 0; i < n; ++i)
    {
        crc ^= d[i];
        for(int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    }
    return crc ^ 0xFFFF;
}

static int aprs_parse(const uint8_t *f, int n, char *out, int outsz)
{
    if(n < 16) return 0;
    char src[12], dst[12];
    char path[64]; path[0] = 0;
    char *names[3] = { dst, src, NULL };
    /* dest (0), src (7) */
    for(int k = 0; k < 2; ++k)
    {
        int o = k * 7, p = 0;
        for(int i = 0; i < 6; ++i) { char c = (char)(f[o+i] >> 1); if(c != ' ') names[k][p++] = c; }
        int ssid = (f[o+6] >> 1) & 0x0f;
        if(ssid) { names[k][p++] = '-'; if(ssid >= 10){names[k][p++]='1';ssid-=10;} names[k][p++] = (char)('0'+ssid); }
        names[k][p] = 0;
    }
    /* digipeater path until the address-end bit, then control(0x03)+pid(0xf0) */
    int o = 14, last = f[13] & 1;
    while(!last && o + 7 <= n)
    {
        char dg[12]; int p = 0;
        for(int i = 0; i < 6; ++i) { char c = (char)(f[o+i] >> 1); if(c != ' ') dg[p++] = c; }
        int ssid = (f[o+6] >> 1) & 0x0f;
        if(ssid) { dg[p++]='-'; if(ssid>=10){dg[p++]='1';ssid-=10;} dg[p++]=(char)('0'+ssid); }
        dg[p] = 0; last = f[o+6] & 1; o += 7;
        if(strlen(path) + p + 2 < sizeof(path)) { strcat(path, ","); strcat(path, dg); }
    }
    if(o + 2 > n || f[o] != 0x03) return 0;          /* not a UI frame          */
    o += 2;                                          /* skip control + PID      */
    int infolen = n - o;
    if(infolen < 0) infolen = 0;
    int w = snprintf(out, outsz, "%s>%s%s:", src, dst, path);
    for(int i = 0; i < infolen && w < outsz - 1; ++i)
    {
        char c = (char)f[o+i];
        out[w++] = (c >= 32 && c < 127) ? c : '.';
    }
    out[w < outsz ? w : outsz-1] = 0;
    return 1;
}

/* Push one fully-assembled HDLC frame (between flags); verify FCS + parse. */
static int aprs_endframe(aprs_t *a, char *out, int outsz)
{
    int got = 0;
    if(a->flen >= 4)
    {
        int n = a->flen - 2;
        uint16_t rx = (uint16_t)a->frame[n] | ((uint16_t)a->frame[n+1] << 8);
#ifdef APRS_HOST_TEST
        fprintf(stderr, "endframe flen=%d fcs_rx=%04x fcs_calc=%04x\n",
                a->flen, rx, crc16_x25(a->frame, n));
#endif
        if(crc16_x25(a->frame, n) == rx)
            got = aprs_parse(a->frame, n, out, outsz);
    }
    a->inframe = 0; a->flen = 0; a->bitcnt = 0; a->cur = 0; a->ones = 0;
    return got;
}

/* Feed one decoded data bit (post-NRZI, post-flag/de-stuff) into the byte
 * assembler. */
static void aprs_databit(aprs_t *a, int bit)
{
    a->cur |= (bit & 1) << a->bitcnt;
    if(++a->bitcnt == 8)
    {
        if(a->flen < APRS_MAXFR) a->frame[a->flen++] = a->cur;
        if(a->flen > a->dbg_maxflen) a->dbg_maxflen = a->flen;
        a->cur = 0; a->bitcnt = 0;
    }
}

/* Feed one raw (pre-NRZI) symbol bit from the bit-timing recovery: detect
 * flags, de-stuff, NRZI-decode, and route data bits to the byte assembler.
 * Returns 1 (with out filled) when a valid frame closes. */
static int aprs_rawbit(aprs_t *a, int sym, char *out, int outsz)
{
    /* NRZI decode: no change -> 1, change -> 0 */
    int dbit = (sym == a->nrzi_last) ? 1 : 0;
    a->nrzi_last = sym;

    /* track last 8 *NRZI-decoded* bits to spot the 0x7E flag (0b01111110) */
    a->shift = ((a->shift << 1) | (uint32_t)dbit) & 0xff;

    int done = 0;
    if(a->shift == 0x7e)                       /* HDLC flag */
    {
        a->dbg_flags++;
#ifdef APRS_HOST_TEST
        fprintf(stderr, "FLAG (inframe=%d flen=%d)\n", a->inframe, a->flen);
#endif
        if(a->inframe) done = aprs_endframe(a, out, outsz);
        a->inframe = 1; a->flen = 0; a->bitcnt = 0; a->cur = 0; a->ones = 0;
        return done;
    }
    if(!a->inframe) return 0;

    if(dbit == 1)
    {
        a->ones++;
        if(a->ones >= 7) { a->inframe = 0; a->ones = 0; return 0; }  /* abort  */
        aprs_databit(a, 1);                    /* a flag's 6 ones land in a    */
    }                                          /* partial byte, dropped below  */
    else
    {
        if(a->ones == 5) { a->ones = 0; return 0; }  /* drop the stuffed 0     */
        a->ones = 0;
        aprs_databit(a, 0);
    }
    return 0;
}

/* Feed raw 8 kHz demod samples.  Returns 1 and fills `out` when a valid AX.25
 * UI frame is decoded (the most recent one within this call). */
int aprs_feed(aprs_t *a, const int16_t *s, int n, char *out, int outsz)
{
    int got = 0;
    for(int k = 0; k < n; ++k)
    {
        /* DC block */
        a->dc += (((int32_t)s[k] << 10) - a->dc) >> 10;
        int16_t x = (int16_t)(s[k] - (a->dc >> 10));

        a->ring[a->rpos] = x;
        a->rpos = (a->rpos + 1) % APRS_NTAP;

        /* non-coherent correlator: mark vs space magnitude */
        int32_t im = 0, qm = 0, is = 0, qs = 0;
        int idx = a->rpos;
        for(int i = 0; i < APRS_NTAP; ++i)
        {
            int v = a->ring[idx];
            im += v * cmark[i]; qm += v * smark[i];
            is += v * cspc [i]; qs += v * sspc [i];
            idx = (idx + 1) % APRS_NTAP;
        }
        int64_t mm = (int64_t)im*im + (int64_t)qm*qm;
        int64_t ms = (int64_t)is*is + (int64_t)qs*qs;
        int sym = (mm > ms) ? 1 : 0;

        /* bit-timing PLL: sample at bit centre, nudge on symbol transitions */
        if(sym != a->lastsym) a->phase = APRS_SPB256 / 2;
        a->lastsym = sym;
        a->phase += 256;
        if(a->phase >= APRS_SPB256)
        {
            a->phase -= APRS_SPB256;
            if(aprs_rawbit(a, sym, out, outsz)) got = 1;
        }
    }
    return got;
}

#ifdef APRS_HOST_TEST
/* Host harness: read s16le samples from argv[1] (or stdin), print decoded
 * frames.  Build: cc -DAPRS_HOST_TEST hd2_aprs.c -o aprs_c && ./aprs_c x.s16 */
int main(int argc, char **argv)
{
    FILE *fp = (argc > 1) ? fopen(argv[1], "rb") : stdin;
    if(!fp) { perror("open"); return 1; }
    aprs_t a; aprs_init(&a);
    int16_t buf[256]; size_t r; int frames = 0; char line[300];
    while((r = fread(buf, 2, 256, fp)) > 0)
        if(aprs_feed(&a, buf, (int)r, line, sizeof line)) { printf("FRAME: %s\n", line); frames++; }
    fprintf(stderr, "decoded %d frame(s)\n", frames);
    return frames ? 0 : 2;
}
#endif
