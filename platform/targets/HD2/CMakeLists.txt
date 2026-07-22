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

# Low-RAM-origin linker (out-of-tree, absolute path -> used verbatim by the
# miosix arch cmake). The stock board unikernel.ld reserves ~76 KiB of RAM for
# the AMBE+2 DMR-voice codec; this radio-free min never uses it, so reclaim that
# window (112 KiB -> 188 KiB app RAM) for the codec2 voice-prompt state. FORCE
# so it overrides any previously-cached board default.
set(MIOSIX_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/hd2_min_lowram.ld
    CACHE STRING "HD2 min-bringup low-RAM-origin linker script" FORCE)

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
    ${ORTX}/openrtx/src/core/voicePromptUtils.c
    ${ORTX}/openrtx/src/core/voicePrompts.c)
# audio_codec.c is replaced by the HD2 pre-decode variant (audio_codec_HD2.c);
# dsp.cpp (encode-side) + rtx.cpp + OpMode_* (radio stack) stay omitted.

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
    ${ORTX}/platform/drivers/audio/codec_HD2.c
    ${ORTX}/platform/drivers/audio/audio_HD2.c
    ${ORTX}/platform/drivers/audio/outputStream_HD2.cpp
    ${CMAKE_SOURCE_DIR}/audio_codec_HD2.c
    ${CMAKE_SOURCE_DIR}/vpdata_HD2.S
    ${ORTX}/platform/mcu/CSKY_V2/drivers/rtc_hd2.c
    ${ORTX}/platform/mcu/CSKY_V2/drivers/adc_hd2.c
    ${ORTX}/platform/drivers/GPS/gps_HD2.c
    ${ORTX}/platform/drivers/GPS/nmea_rbuf.c)

# --- Vendored helper libs the core pulls in ---------------------------------
set(ORTX_LIBS
    ${ORTX}/lib/minmea/minmea.c
    ${ORTX}/lib/QRCode/qrcode.c)

# --- Codec2-mod (M17 3200, fixed-point) -- voice-prompt decode + benchmark ---
# The M17-Project Codec2-mod fork (under hd2-clean/vendor), a minimal malloc-free
# 3200-only refactor with a STATIC fixed-size codec2_t (~29 KB .bss).  C2_FIXED
# selects the on-device fixed-point encode/decode path.  Path is ${ORTX}-relative
# so it resolves under both the host tree and the /work docker mount.
set(C2M ${ORTX}/../hd2-clean/vendor/Codec2-mod)
add_library(codec2_hd2 STATIC
    ${C2M}/src/analysis.c   ${C2M}/src/codec2_mod.c ${C2M}/src/delta_lsp_cb.c
    ${C2M}/src/interp.c     ${C2M}/src/kiss_fft.c   ${C2M}/src/kiss_fftr.c
    ${C2M}/src/lpc.c        ${C2M}/src/nlp.c        ${C2M}/src/quantise.c
    ${C2M}/src/synthesis.c  ${C2M}/src/util.c       ${C2M}/src/fft_fixed.c)
target_include_directories(codec2_hd2 PRIVATE ${C2M}/inc)
target_compile_options(codec2_hd2 PRIVATE
    -w -ffunction-sections -fdata-sections
    -fsingle-precision-constant -fno-math-errno
    -Os) # size-opt: shrinks the decode working set to fit the 4 KB I-cache
         # (~3.9 KB), halving aks2mag2 vs -O2. Requires the codec2 linker pin.
    # NOTE: do NOT add -mdsp here. It replaces the decode's __muldi3 64-bit
    # multiplies with C-SKY DSP instructions, but the HR_C7000 CK803S does NOT
    # implement the DSP extension -- verified 2026-07-22: encode (no DSP insns)
    # ran fine while decode (DSP insns) faulted -> bootloop. -mcpu=ck803s is a
    # no-op in this gcc (identical codegen to ck803). Decode stays ~31.7 ms.
# NOTE: AKS_FFT_NOPRUNE is deliberately NOT defined -- leaving it out selects the
# bit-identical input-pruned aks FFT (only 11 of 256 inputs nonzero), the faster
# path. C2_PROF enables the per-stage prof_mark() decode profiler.
target_compile_definitions(codec2_hd2 PRIVATE C2_FIXED)

add_executable(openrtx_hd2
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${CMAKE_SOURCE_DIR}/hd2_glue.cpp
    ${CMAKE_SOURCE_DIR}/hd2_stubs.c
    ${ORTX_CORE} ${ORTX_UI} ${ORTX_HD2} ${ORTX_LIBS})

target_include_directories(openrtx_hd2 PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ORTX}/openrtx/include
    ${ORTX}/platform
    ${ORTX}/platform/targets/HD2
    ${ORTX}/platform/mcu/CSKY_V2
    ${ORTX}/lib/minmea/include
    ${ORTX}/lib/QRCode/include
    ${ORTX}/lib/qdec/include
    ${C2M}/inc)

target_link_libraries(openrtx_hd2 PRIVATE codec2_hd2)

# The voice-prompt blob embed (vpdata_HD2.S) resolves `.incbin "voiceprompts.vpc"`
# via an assembler include path pointing at the OpenRTX repo root (${ORTX}), where
# voiceprompts.vpc lives.
set_source_files_properties(${CMAKE_SOURCE_DIR}/vpdata_HD2.S PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${ORTX}")

target_compile_definitions(openrtx_hd2 PRIVATE
    PLATFORM_HD2 _MIOSIX FONT_UBUNTU_REGULAR timegm=mktime
    "GIT_VERSION=\"openrtx-hd2-min\"")

miosix_link_target(openrtx_hd2)
