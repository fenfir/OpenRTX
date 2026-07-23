/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenRTX-on-Miosix entry for the Ailunce HD2 (HR_C7000 / CK803S), minimal
 * bring-up.  The vendor Miosix kernel starts us as the main thread after
 * kernel init, so we hand straight off to OpenRTX's standard threaded flow.
 */

extern "C" void openrtx_init(void);
extern "C" void *openrtx_run(void *arg);

int main()
{
    openrtx_init();       // platform/state/gfx/kbd/ui + codeplug + splash
    openrtx_run(nullptr); // create_threads() (ui + rtx) then main_thread loop
    return 0;
}
