/*
 * SPDX-FileCopyrightText: Copyright 2026 HD2 Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * !!! WORK IN PROGRESS -- DO NOT CONSUME !!!
 *
 * Cooperative OpenRTX UI superloop for the HD2 bring-up.
 *
 * This is the "make it work" half of the loader->app switch (the
 * "make it real" half is the miosix C-SKY port, now proven viable by
 * the periodic-IRQ test).  Instead of create_threads() + pthreads, we
 * run the bodies of OpenRTX's ui_threadFunc + main_thread interleaved
 * in ONE loop, paced by the real 42 MHz getTick/sleepUntil.  The RTX
 * radio thread is omitted (no radio path yet); rtx_* are stubbed.
 *
 * Entered from the loader 'A' command so the loader boots first (and
 * stays probeable): we call loader_cmd_poll() every iteration, so R/W/
 * keys/JEDEC/etc. keep working WHILE the UI runs -- our live debug
 * window into the running app.
 */

#include "interfaces/platform.h"
#include "interfaces/display.h"
#include "interfaces/keyboard.h"
#include "interfaces/delays.h"
#include "core/graphics.h"
#include "core/state.h"
#include "core/ui.h"
#include "core/input.h"
#include "core/event.h"
#include "core/voicePrompts.h"
#include "loader_cmd.h"
#include <stdbool.h>

#ifdef CONFIG_GPS
#include "core/gps.h"
#endif

void hd2_app_run(void)
{
    static bool running = false;
    if (running)          /* 'A' is idempotent: don't re-enter recursively */
        return;
    running = true;

    /* platform_init() + kbd_init() already ran in the loader boot.
     * Mirror the rest of openrtx_init()'s UI bring-up. */
    state_init();
    gfx_init();
    ui_init();
    vp_init();                       /* stubbed no-op (no audio path yet) */

    /* Splash, then backlight on (hides render garbage), per openrtx_init. */
    ui_drawSplashScreen();
    gfx_render();
    sleepFor(0u, 30u);
    display_setBacklightLevel(state.settings.brightness);

    /* Cooperative merge of ui_threadFunc + main_thread (see core/threads.c).
     * No rtx thread: we skip the rtx_configure() sync block entirely. */
    kbd_msg_t kbd_msg;
    bool      sync_rtx = true;

#ifdef CONFIG_GPS
    const struct gpsDevice *gps = platform_initGps();
#endif

    ui_saveState();
    ui_updateGUI();

    long long t = getTick();
    while (1) {
        loader_cmd_poll();           /* keep the loader/probe live */

        if (input_scanKeyboard(&kbd_msg))
            ui_pushEvent(EVENT_KBD, kbd_msg.value);

        ui_updateFSM(&sync_rtx);     /* (sync_rtx ignored: no radio) */
        ui_saveState();

        if (ui_updateGUI() == true)
            gfx_render();

        state_task();

#ifdef CONFIG_GPS
        gps_task(gps);   /* drains UART2 NMEA into state.gps_data */
#endif

        /* Power/volume knob turned off -> graceful shutdown.  This is the
         * main_thread power-off check (core/threads.c) folded into the
         * superloop.  Debounce the detent, then cut the power latch. */
        if (platform_pwrButtonStatus() == false) {
            sleepFor(0u, 50u);
            if (platform_pwrButtonStatus() == false) {
                state.devStatus = SHUTDOWN;
                display_setBacklightLevel(0);   /* screen dark before the rail drops */
                platform_terminate();           /* releases GPIOB.13 -- never returns */
            }
        }

        t += 25;                     /* ~40 Hz UI/keyboard cadence */
        sleepUntil(t);
    }
}
