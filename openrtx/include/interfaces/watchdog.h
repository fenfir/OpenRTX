/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * System failsafe watchdog hook, kicked once per RTX-thread iteration.
 *
 * Targets with a hardware watchdog implement this to arm it (on the first kick)
 * and feed it; if the RTX thread / scheduler hangs the kicks stop and the
 * watchdog hardware-resets the radio.  The default (weak) implementation is a
 * no-op, so targets without a watchdog are unaffected.
 *
 * Called from rtx_threadFunc()'s loop, so it tracks "the system is still
 * scheduling" -- the dominant lock class (scheduler death) stops it and trips
 * the reset.
 */
void watchdog_kick(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H */
