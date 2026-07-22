/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Embeds the codec2 voice-prompt blob (voiceprompts.vpc) that core/voicePrompts.c
 * reads via _vpdata_start/_vpdata_end. The .vpc is resolved through the assembler
 * -I set on this file in CMakeLists (points at the OpenRTX repo root).
 */
.section .note.GNU-stack,""
.section .rodata

.global _vpdata_start
.global _vpdata_end

_vpdata_start:
    .incbin "voiceprompts.vpc"
.set _vpdata_end, .
