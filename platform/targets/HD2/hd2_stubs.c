/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stubs for the subsystems the minimal HD2 bring-up leaves out (radio / DMR /
 * audio / voice prompts / codeplug NVM / rtx task).  They let the OpenRTX core
 * and default UI link so the display / backlight / keyboard / RTC milestone
 * runs.  Each is replaced by a real driver as that subsystem is brought up.
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "core/voicePrompts.h"
#include "interfaces/cps_io.h"
#include "interfaces/nvmem.h"
#include "rtx/rtx.h"

/* Miosix owns the timebase, so OpenRTX's bare-metal timer init is a no-op. */
void timer_init(void)
{
}

/* Voice prompts are now real (core/voicePrompts.c + audio_codec_HD2.c); their
 * stubs were removed. */

/* ---- Codeplug NVM (no W25Q reader): open succeeds with an empty codeplug,
 *      per-record reads return -1 so the core uses its built-in defaults. --- */
int cps_open(char *cps_name)
{
    (void)cps_name;
    return 0;
}
int cps_create(char *cps_name)
{
    (void)cps_name;
    return 0;
}
int cps_readChannel(channel_t *channel, uint16_t pos)
{
    (void)channel;
    (void)pos;
    return -1;
}
int cps_readBankHeader(bankHdr_t *b_header, uint16_t pos)
{
    (void)b_header;
    (void)pos;
    return -1;
}
int cps_readBankData(uint16_t bank_pos, uint16_t pos)
{
    (void)bank_pos;
    (void)pos;
    return -1;
}
int cps_readContact(contact_t *contact, uint16_t pos)
{
    (void)contact;
    (void)pos;
    return -1;
}

/* ---- Settings/VFO NVM (no W25Q store): use defaults, writes are dropped --- */
int nvm_readSettings(settings_t *settings)
{
    (void)settings;
    return -1;
}
int nvm_readVfoChannelData(channel_t *channel)
{
    (void)channel;
    return -1;
}
int nvm_writeSettingsAndVfo(const settings_t *settings, const channel_t *vfo)
{
    (void)settings;
    (void)vfo;
    return 0;
}

/* ---- rtx (no radio task in the minimal bring-up) -------------------------- */
extern void sleepFor(unsigned int seconds, unsigned int mseconds);

void rtx_init(pthread_mutex_t *m)
{
    (void)m;
}
void rtx_terminate()
{
}
/* rtx_threadFunc spins `while(RUNNING) rtx_task();` -- sleep so the empty
 * task yields the CPU to the UI/main threads instead of busy-looping. */
void rtx_task()
{
    sleepFor(0, 100);
}
void rtx_configure(const rtxStatus_t *cfg)
{
    (void)cfg;
}
bool rtx_rxSquelchOpen()
{
    return false;
}
rssi_t rtx_getRssi()
{
    return (rssi_t)-127;
}
