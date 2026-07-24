/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Glue between OpenRTX and the vendor Miosix kernel for the HD2.  The
 * interfaces/delays.h implementation lives in mcu/HR_C7000/drivers/delays.cpp;
 * this file only carries the raw bring-up UART trace helper + the codec2
 * decode benchmark print.
 */

#include <miosix.h>

extern "C" {

// Raw UART0 trace for bring-up (UART0 @ 0x14030000, 57600 8N1 set by the bsp).
// Bounded spin so a stuck UART can't hang a thread.
void hd2_trace(const char *s)
{
    volatile unsigned int *thr = (volatile unsigned int *)0x14030000u;
    volatile unsigned int *lsr = (volatile unsigned int *)0x14030014u;
    while (*s) {
        for (unsigned int g = 0; g < 200000u && (*lsr & 0x20u) == 0u; ++g) {
        }
        *thr = (unsigned char)*s++;
    }
}

// Monotonic nanoseconds since boot (tickless vendor Miosix clock).
unsigned long long hd2_time_ns(void)
{
    return (unsigned long long)miosix::getTime();
}

// Append an unsigned decimal to *p, return the new write pointer.
static char *hd2_u2dec(char *p, unsigned long long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0ull)
        tmp[i++] = '0';
    while (v) {
        tmp[i++] = (char)('0' + (v % 10ull));
        v /= 10ull;
    }
    while (i)
        *p++ = tmp[--i];
    return p;
}

// codec2 decode benchmark line: "c2dec min=<us>us avg=<us>us max=<us>us n=<n>".
void hd2_bench_c2(unsigned long long min_ns, unsigned long long sum_ns,
                  unsigned long long max_ns, unsigned int n)
{
    if (n == 0u)
        return;
    char buf[96];
    char *p = buf;
    for (const char *a = "c2dec min="; *a;)
        *p++ = *a++;
    p = hd2_u2dec(p, min_ns / 1000ull);
    for (const char *a = "us avg="; *a;)
        *p++ = *a++;
    p = hd2_u2dec(p, (sum_ns / n) / 1000ull);
    for (const char *a = "us max="; *a;)
        *p++ = *a++;
    p = hd2_u2dec(p, max_ns / 1000ull);
    for (const char *a = "us n="; *a;)
        *p++ = *a++;
    p = hd2_u2dec(p, n);
    *p++ = '\n';
    *p = '\0';
    hd2_trace(buf);
}

} // extern "C"
