/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * !!! WORK IN PROGRESS -- DO NOT CONSUME !!!
 *
 * Stubs that let the OpenRTX core (graphics + ui + state) link for the
 * HD2 superloop bring-up WITHOUT pulling in subsystems we haven't ported
 * yet:
 *   - voice prompts (vp_*): the real voicePrompts.c drags in the codec
 *     (codec2) + the audio path.  No audio driver yet, so no-op them.
 *   - rtx (rtx_*): the RTX thread is not wired in the superloop.  The
 *     low-level radio.h driver now EXISTS (radio_HD2.cpp: AT1846S FM-RX
 *     control path -- tune/RSSI/CTCSS-squelch), but the full rtx.cpp
 *     state machine that would call it drags in OpMode_M17 + codec2 +
 *     the audio path, none of which are ported yet.  So rtx_* stay
 *     stubbed here (benign OFF status) until the audio agent lands the
 *     audio path and we can link the real rtx.cpp / OpMode_FM.  At that
 *     point: drop these three stubs, add openrtx/src/rtx/*.cpp to the
 *     meson source list, and run radio_init/radio_updateConfiguration
 *     from the superloop (or a real rtx_task()).
 *
 * As each subsystem comes online (audio -> real vp; rtx -> real radio),
 * delete the matching stubs here.
 *
 * NOTE: the codeplug / NVM backend (cps_read* and nvm_read*) has been
 * promoted out of this file into the real driver
 * platform/drivers/NVM/nvmem_HD2.c (OpenRTX-native codeplug on the W25Q
 * via `eflash`).  It is inert-safe (returns -1 -> core defaults) until
 * the firmware-side W25Q read is fixed (#73), so behaviour is unchanged
 * for now.
 */

#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"
#include "rtx/rtx.h"
#include "core/memory_profiling.h"
#include <string.h>

/* ---- voice prompts: no-op (no audio path yet) --------------------- */
void vp_init(void) {}
void vp_terminate(void) {}
void vp_stop(void) {}
void vp_flush(void) {}
void vp_play(void) {}
void vp_tick(void) {}
bool vp_isPlaying(void) { return false; }
bool vp_sequenceNotEmpty(void) { return false; }
void vp_queuePrompt(const uint16_t prompt) { (void)prompt; }
void vp_queueString(const char *string, enum vpFlags flags) { (void)string; (void)flags; }
void vp_queueInteger(const int value) { (void)value; }
void vp_queueStringTableEntry(const char *const *s) { (void)s; }
void vp_queueFrequency(const freq_t freq) { (void)freq; }
void vp_beep(uint16_t freq, uint16_t duration) { (void)freq; (void)duration; }
void vp_beepSeries(const uint16_t *beepSeries) { (void)beepSeries; }
enum vpQueueFlags vp_getVoiceLevelQueueFlags(void) { return (enum vpQueueFlags)0; }
void vp_replayLastPrompt(void) {}
void vp_playMenuBeepIfNeeded(bool firstItem) { (void)firstItem; }

void vp_announceChannelName(const channel_t *channel, const uint16_t channelNumber, const enum vpQueueFlags flags) { (void)channel; (void)channelNumber; (void)flags; }
void vp_announceFrequencies(const freq_t rx, const freq_t tx, const enum vpQueueFlags flags) { (void)rx; (void)tx; (void)flags; }
void vp_announceRadioMode(const uint8_t mode, const enum vpQueueFlags flags) { (void)mode; (void)flags; }
void vp_announceBandwidth(const uint8_t bandwidth, const enum vpQueueFlags flags) { (void)bandwidth; (void)flags; }
void vp_announceChannelSummary(const channel_t *channel, const uint16_t channelNumber, const uint16_t bank, const enum vpSummaryInfoFlags infoFlags) { (void)channel; (void)channelNumber; (void)bank; (void)infoFlags; }
void vp_announceInputChar(const char ch) { (void)ch; }
void vp_announceInputReceiveOrTransmit(const bool tx, const enum vpQueueFlags flags) { (void)tx; (void)flags; }
void vp_announceError(const enum vpQueueFlags flags) { (void)flags; }
void vp_announceText(const char *text, const enum vpQueueFlags flags) { (void)text; (void)flags; }
void vp_announceCTCSS(const bool rxToneEnabled, const uint8_t rxTone, const bool txToneEnabled, const uint8_t txTone, const enum vpQueueFlags flags) { (void)rxToneEnabled; (void)rxTone; (void)txToneEnabled; (void)txTone; (void)flags; }
void vp_announcePower(const uint32_t power, const enum vpQueueFlags flags) { (void)power; (void)flags; }
void vp_announceSquelch(const uint8_t squelch, const enum vpQueueFlags flags) { (void)squelch; (void)flags; }
void vp_announceContact(const contact_t *contact, const enum vpQueueFlags flags) { (void)contact; (void)flags; }
bool vp_announceContactWithIndex(const uint16_t index, const enum vpQueueFlags flags) { (void)index; (void)flags; return false; }
void vp_announceTimeslot(const uint8_t timeslot, const enum vpQueueFlags flags) { (void)timeslot; (void)flags; }
void vp_announceColorCode(const uint8_t rxColorCode, const uint8_t txColorCode, const enum vpQueueFlags flags) { (void)rxColorCode; (void)txColorCode; (void)flags; }
void vp_announceBank(const uint16_t bank, const enum vpQueueFlags flags) { (void)bank; (void)flags; }
void vp_announceM17Info(const channel_t *channel, bool isEditing, const enum vpQueueFlags flags) { (void)channel; (void)isEditing; (void)flags; }
void vp_announceGPSInfo(enum vpGPSInfoFlags gpsInfoFlags) { (void)gpsInfoFlags; }
void vp_announceAboutScreen(void) {}
void vp_announceBackupScreen(void) {}
void vp_announceRestoreScreen(void) {}
void vp_announceSettingsTimeDate(void) {}
void vp_announceSettingsVoiceLevel(const enum vpQueueFlags flags) { (void)flags; }
void vp_announceSettingsOnOffToggle(const char *const *s, const enum vpQueueFlags flags, bool val) { (void)s; (void)flags; (void)val; }
void vp_announceSettingsInt(const char *const *s, const enum vpQueueFlags flags, int val) { (void)s; (void)flags; (void)val; }
void vp_announceScreen(uint8_t ui_screen) { (void)ui_screen; }
void vp_announceBuffer(const char *const *s, bool editMode, bool callsign, const char *buffer) { (void)s; (void)editMode; (void)callsign; (void)buffer; }
void vp_announceDisplayTimer(void) {}
void vp_announceSplashScreen(void) {}
void vp_announceTimeZone(const int8_t timeZone, const enum vpQueueFlags flags) { (void)timeZone; (void)flags; }

/* ---- rtx: no radio thread in the superloop ------------------------ */
rtxStatus_t rtx_getCurrentStatus(void)
{
    rtxStatus_t s;
    memset(&s, 0, sizeof(s));
    s.opMode    = OPMODE_FM;
    s.opStatus  = OFF;
    return s;
}

bool rtx_rxSquelchOpen(void) { return false; }
rssi_t rtx_getRssi(void) { return -127; }

/* ---- heap: stub until profiling wired ----------------------------- *
 * battery_getCharge() now has a real implementation in
 * platform/targets/HD2/platform.c (vendor linear curve). */
unsigned int getHeapSize(void) { return 0; }
unsigned int getCurrentFreeHeap(void) { return 0; }
