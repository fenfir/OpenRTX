/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/openrtx.h"

#ifdef PLATFORM_MD9600
#include "interfaces/platform.h"
#include "interfaces/delays.h"
#endif

#ifdef PLATFORM_LINUX
#include "emulator/sdl_engine.h"
#endif

#ifndef PLATFORM_HD2
/* HD2 supplies its own main() in platform/targets/HD2/main.c during
 * the first bring-up phase -- it bypasses openrtx_init() until the
 * driver stubs for display/keyboard/threads/UI/voice prompts have
 * working HD2-specific implementations.  Once they do, this ifdef
 * guard goes away and HD2 joins the standard flow.
 */
int main(void)
{
    // MD-9600 does not have a proper power on/off mechanism and the MCU is
    // always powered on. We thus need to place a busy wait on the power on
    // button to manage the on/off mechanism.
    // A do-while block is used to avoid re-powering on after a power off due to
    // MCU rebooting before user stops pressing the power button.
    #ifdef PLATFORM_MD9600
    do
    {
        sleepFor(1, 0);
    }
    while(!platform_pwrButtonStatus());
    #endif

    openrtx_init();

#ifndef PLATFORM_LINUX
    openrtx_run(NULL);
#else
    // macOS requires SDL main loop to run on the main thread.
    // Here we create a new thread for OpenRTX main program and utilize the main
    // thread for the SDL main loop.
    pthread_t openrtx_thread;
    pthread_create(&openrtx_thread, NULL, openrtx_run, NULL);

    sdlEngine_run();
    pthread_join(openrtx_thread, NULL);
#endif
}
#endif /* !PLATFORM_HD2 */
