/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * !!! WORK IN PROGRESS -- DO NOT CONSUME !!!
 *
 * Loader command module interface.  The peek/poke loader's UART command
 * dispatch (R/W/P/Z/G/T/B/K/k/J/j/D/V/l/8/t/i/N/n/S/E/U/?) is factored behind this
 * tiny interface so it can run from BOTH the bare loader loop (main.c)
 * AND the OpenRTX app superloop -- i.e. we keep full MMIO/keypad/probe
 * access while the UI runs.  Implementation currently lives in main.c.
 */
#ifndef HD2_LOADER_CMD_H
#define HD2_LOADER_CMD_H

#include <stdint.h>

/* Handle one command byte (reads any following arg bytes off UART). */
void loader_cmd_dispatch(uint8_t c);

/* Non-blocking: if a command byte is pending on UART0, consume + dispatch
 * it and return 1; otherwise return 0.  Call this each iteration of any
 * main/app loop to keep the loader live. */
int loader_cmd_poll(void);

/* Print the help banner. */
void loader_cmd_banner(void);

#endif /* HD2_LOADER_CMD_H */
