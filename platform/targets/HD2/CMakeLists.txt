# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# OpenRTX minimal bring-up for the Ailunce HD2 (HR_C7000 / CK803S), built on
# the vendor Miosix kernel consumed as a dependency.  This build tree lives in
# the OpenRTX target dir; the miosix repo carries no OpenRTX/app code.
#
# Scope: display + backlight + keyboard + RTC + battery + GPS + the UI.  No
# radio / DMR / audio / M17 / NVM -- those are stubbed (hd2_stubs.c) so the
# core + UI link.
cmake_minimum_required(VERSION 3.21)

# Kernel dependency.  Normally miosix-kernel is a sibling of OpenRTX under
# vendor/ (../../../../miosix-kernel/miosix).  This worktree lives outside that
# tree, so MIOSIX_KPATH defaults to the known checkout and is overridable:
#   cmake -DMIOSIX_KPATH=/path/to/miosix-kernel/miosix ...
set(MIOSIX_KPATH "/Users/zach/Documents/fenfir/hd1/hd2-clean/vendor/miosix-kernel/miosix"
    CACHE PATH "Path to the miosix kernel tree")
set(CMAKE_TOOLCHAIN_FILE ${MIOSIX_KPATH}/cmake/Toolchains/gcc-csky.cmake)
set(CMAKE_BUILD_TYPE "Release")

project(OpenRtxHd2Min C CXX ASM)

set(MIOSIX_BOARD hrc7000_hd2)
set(MIOSIX_USER_CONFIG_PATH ${MIOSIX_KPATH}/config)

# Custom application linker script: pins the codec2 decode .text into the CK803S
# I-cache (see hd2_codec2.ld).  The kernel uses MIOSIX_LINKER_SCRIPT directly
# when it is an absolute path (miosix/arch/CMakeLists.txt), so force it here
# before the kernel subdir sets its own default (unikernel.ld).
set(MIOSIX_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/hd2_codec2.ld"
    CACHE STRING "Linker script" FORCE)

add_subdirectory(${MIOSIX_KPATH} miosix EXCLUDE_FROM_ALL)

# OpenRTX repo root: this dir is <root>/platform/targets/HD2, so up 3.
set(ORTX ${CMAKE_SOURCE_DIR}/../../..)

# --- Hardware-agnostic OpenRTX core (boot spine + graphics + input) ---------
# Mirrors the proven HD2 core list.  Kept intact rather than trimmed further:
# the boot threshold is a ~130 KB image (heavily-stripped images don't boot on
# this SoC), and trimming these only multiplies the stub surface below.
set(ORTX_CORE
    ${ORTX}/openrtx/src/core/state.c
    ${ORTX}/openrtx/src/core/threads.c
    ${ORTX}/openrtx/src/core/battery.c
    ${ORTX}/openrtx/src/core/graphics.c
    ${ORTX}/openrtx/src/core/input.c
    ${ORTX}/openrtx/src/core/utils.c
    ${ORTX}/openrtx/src/core/queue.c
    ${ORTX}/openrtx/src/core/chan.c
    ${ORTX}/openrtx/src/core/gps.c
    ${ORTX}/openrtx/src/core/cps.c
    ${ORTX}/openrtx/src/core/crc.c
    ${ORTX}/openrtx/src/core/datetime.c
    ${ORTX}/openrtx/src/core/openrtx.c
    ${ORTX}/openrtx/src/core/data_conversion.c
    ${ORTX}/openrtx/src/core/nvmem_access.c
    ${ORTX}/openrtx/src/core/memory_profiling.cpp
    ${ORTX}/openrtx/src/core/audio_path.cpp
    ${ORTX}/openrtx/src/core/audio_stream.c
    ${ORTX}/openrtx/src/core/voicePromptUtils.c)
# Voice prompts use the stock codec2 player (ORTX_VP below) decoded by the
# custom fixed-point Codec2-mod fork (libcodec2_hd2.a), whose decode .text is
# pinned into the CK803S I-cache by hd2_codec2.ld.  So audio_codec.c + dsp.cpp
# (ORTX_CODEC) are built here.  The analog-FM radio stack (rtx.cpp + OpMode_FM +
# the HD2 baseband/audio drivers) is also built -- see ORTX_RADIO.

# --- Default UI -------------------------------------------------------------
set(ORTX_UI
    ${ORTX}/openrtx/src/ui/default/ui.c
    ${ORTX}/openrtx/src/ui/default/ui_main.c
    ${ORTX}/openrtx/src/ui/default/ui_menu.c
    ${ORTX}/openrtx/src/ui/default/ui_strings.c)

# --- HD2 platform + the four minimal drivers --------------------------------
set(ORTX_HD2
    ${ORTX}/platform/targets/HD2/platform.c
    ${ORTX}/platform/targets/HD2/hwconfig.c
    ${ORTX}/platform/drivers/display/ST7735S_HD2.c
    ${ORTX}/platform/drivers/keyboard/keyboard_HD2.c
    ${ORTX}/platform/drivers/backlight/backlight_HD2.c
    ${ORTX}/platform/drivers/GPIO/gpio_hrc7000.c
    ${ORTX}/platform/mcu/HR_C7000/drivers/rtc_hd2.c
    ${ORTX}/platform/drivers/ADC/adcHrc7000.c
    ${ORTX}/platform/mcu/HR_C7000/drivers/delays.cpp
    ${ORTX}/platform/drivers/GPS/gps_HD2.c
    ${ORTX}/platform/drivers/GPS/nmea_rbuf.c)

# --- Shared driver stubs for subsystems the minimal build leaves out --------
# Codeplug + settings NVM have generic OpenRTX stubs; use them rather than
# hand-rolled copies.  Radio / audio / stream stubs are NOT needed: the core
# never calls those wrappers in this build, so --gc-sections drops them.
set(ORTX_STUBS
    ${ORTX}/platform/drivers/stubs/cps_io_stub.c
    ${ORTX}/platform/drivers/stubs/nvmem_stub.c)

# --- Analog-FM radio stack --------------------------------------------------
# The hardware-agnostic radio task (rtx.cpp) + the FM operating mode
# (OpMode_FM.cpp) driving the HD2 baseband/audio drivers: AT1846S transceiver,
# HR_C7000 codec/modem FM gate (radio_HD2.cpp), the board audio-routing matrix
# (audio_HD2.c) and the I2C1 radio bus (i2c1_hd2.c).  FM only -- no DMR / M17 /
# broadcast-FM.  rtx.cpp #includes OpMode_M17.hpp unconditionally, but the M17
# object is CONFIG_M17-gated, so OpMode_M17.cpp is not needed here.
set(ORTX_RADIO
    ${ORTX}/openrtx/src/rtx/rtx.cpp
    ${ORTX}/openrtx/src/rtx/OpMode_FM.cpp
    ${ORTX}/platform/drivers/baseband/radio_HD2.cpp
    ${ORTX}/platform/drivers/baseband/AT1846S_HD2.cpp
    ${ORTX}/platform/drivers/audio/audio_HD2.c
    ${ORTX}/platform/drivers/audio/outputStream_HD2.cpp
    ${ORTX}/platform/mcu/HR_C7000/drivers/i2c1_hd2.c)

# --- Vendored helper libs the core pulls in ---------------------------------
set(ORTX_LIBS
    ${ORTX}/lib/minmea/minmea.c
    ${ORTX}/lib/QRCode/qrcode.c)

# --- Custom fixed-point codec2 (M17 Codec2-mod fork) as a pinned static lib --
# Built as its own archive (libcodec2_hd2.a) so hd2_codec2.ld can pin its .text
# into the CK803S 4 KB I-cache.  -DC2_FIXED selects the fixed-point decode path
# and CHANGES the codec2_t struct layout, so it must be uniform across the whole
# archive (incl. codec2_compat.c, which sizeof()s + inits the struct).  Built -Os
# with a monolithic .text; hd2_codec2.ld pins that .text to the CK803S 4 KB
# I-cache boundary.  Pinned at commit 2e50cc6 of the fork.
set(CODEC2_MOD ${ORTX}/../Codec2-mod)
add_library(codec2_hd2 STATIC
    ${CODEC2_MOD}/src/analysis.c
    ${CODEC2_MOD}/src/codec2_mod.c
    ${CODEC2_MOD}/src/delta_lsp_cb.c
    ${CODEC2_MOD}/src/fft_fixed.c
    ${CODEC2_MOD}/src/interp.c
    ${CODEC2_MOD}/src/kiss_fft.c
    ${CODEC2_MOD}/src/kiss_fftr.c
    ${CODEC2_MOD}/src/lpc.c
    ${CODEC2_MOD}/src/nlp.c
    ${CODEC2_MOD}/src/quantise.c
    ${CODEC2_MOD}/src/synthesis.c
    ${CODEC2_MOD}/src/util.c
    ${CMAKE_CURRENT_SOURCE_DIR}/codec2_compat.c)
target_include_directories(codec2_hd2 PRIVATE
    ${CODEC2_MOD}/inc
    ${CMAKE_CURRENT_SOURCE_DIR})           # codec2.h shim
target_compile_definitions(codec2_hd2 PRIVATE C2_FIXED)
# -Os with a MONOLITHIC .text (NO -ffunction-sections).  The decode hot loops are
# I-cache-set-collision-sensitive and the fork's layout is hand-tuned (Codec2-mod
# 2e50cc6) to keep aks_to_mag2 off a colliding set with lpc.o byte-identical at
# its natural address.  -O2 or -ffunction-sections resize/reorder the functions
# and reshuffle the set mapping -- measured ~6 ms/frame worse on device.  Keep
# -Os and let hd2_codec2.ld pin the archive to a 4 KB boundary for a
# deterministic base.
target_compile_options(codec2_hd2 PRIVATE -Os)

# --- Voice prompts (codec2, decoded by the fork above) ----------------------
# The stock codec2 player (core/voicePrompts.c) + the core clip container embed
# (voicePromptData.S -> repo-root voiceprompts.vpc, codec2 3200).  The .S does
# `.incbin "../voiceprompts.vpc"`; GAS resolves it via an assembler -I of
# ${ORTX}/openrtx.  audio_codec.c/dsp.cpp (ORTX_CODEC) are the façade + DSP the
# player pushes 8-byte codec2 frames through; audio_codec.c's `#include
# "codec2.h"` resolves to the HD2 shim (platform/targets/HD2/codec2.h).
set(ORTX_VP
    ${ORTX}/openrtx/src/core/voicePrompts.c
    ${ORTX}/openrtx/src/core/voicePromptData.S)
set_source_files_properties(${ORTX}/openrtx/src/core/voicePromptData.S
    PROPERTIES COMPILE_OPTIONS "-Wa,-I${ORTX}/openrtx")
set(ORTX_CODEC
    ${ORTX}/openrtx/src/core/audio_codec.c
    ${ORTX}/openrtx/src/core/dsp.cpp)

add_executable(openrtx_hd2
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${CMAKE_SOURCE_DIR}/hd2_glue.cpp
    ${ORTX_CORE} ${ORTX_UI} ${ORTX_HD2} ${ORTX_RADIO} ${ORTX_STUBS}
    ${ORTX_VP} ${ORTX_CODEC} ${ORTX_LIBS})

target_include_directories(openrtx_hd2 PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ORTX}/openrtx/include
    ${ORTX}/platform
    ${ORTX}/platform/targets/HD2
    ${ORTX}/platform/mcu/HR_C7000
    ${CODEC2_MOD}/inc
    ${ORTX}/lib/minmea/include
    ${ORTX}/lib/QRCode/include
    ${ORTX}/lib/codec/include
    ${ORTX}/lib/qdec/include)

target_compile_definitions(openrtx_hd2 PRIVATE
    PLATFORM_HD2 _MIOSIX FONT_UBUNTU_REGULAR timegm=mktime
    "GIT_VERSION=\"openrtx-hd2-vpc2\"")

target_link_libraries(openrtx_hd2 PRIVATE codec2_hd2)

miosix_link_target(openrtx_hd2)
