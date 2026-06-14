/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "core/cps.h"

const uint16_t ctcss_tone[CTCSS_FREQ_NUM] =
{
    670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035,
    1072, 1109, 1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567,
    1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928,
    1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541
};

/*
 * Standard DCS codes as 9-bit octal values (e.g. 0023 -> 19); display as octal
 * ("%03o").  Order/contents taken from the HD2 vendor firmware's DCS table so
 * indices line up with the factory codeplug.  Each value Golay-encodes to the
 * AT1846S CDCSS codeword via AT1846S::dcsCodeword().
 */
const uint16_t dcs_code[DCS_CODE_NUM] =
{
     19,  21,  22,  25,  26,  30,  35,  39,  41,  43,
     44,  53,  57,  58,  59,  60,  76,  77,  78,  82,
     85,  89,  90,  92,  99, 101, 106, 109, 110, 114,
    117, 122, 124, 133, 138, 147, 149, 150, 163, 164,
    165, 166, 169, 170, 173, 177, 179, 181, 182, 185,
    188, 198, 201, 205, 213, 217, 218, 227, 230, 233,
    238, 244, 245, 249, 265, 266, 267, 275, 281, 282,
    293, 294, 308, 298, 300, 301, 306, 309, 310, 323,
    326, 334, 339, 342, 346, 358, 373, 390, 394, 404,
    407, 409, 410, 421, 428, 434, 436, 451, 458, 467,
    473, 474, 476, 483, 492
};

channel_t cps_getDefaultChannel()
{
    channel_t channel;

    #ifdef PLATFORM_MOD17
    channel.mode      = OPMODE_M17;
    #else
    channel.mode      = OPMODE_FM;
    #endif
    channel.bandwidth = BW_25;
    channel.power     = 1000;   // 1W
    channel.rx_only   = false;  // Enable tx by default

    // Set initial frequency based on supported bands
    const hwInfo_t* hwinfo  = platform_getHwInfo();
    if(hwinfo->uhf_band)
    {
        channel.rx_frequency = 430000000;
        channel.tx_frequency = 430000000;
    }
    else if(hwinfo->vhf_band)
    {
        channel.rx_frequency = 144000000;
        channel.tx_frequency = 144000000;
    }

    channel.fm.rxToneEn = 0; //disabled
    channel.fm.rxTone   = 0; //and no ctcss/dcs selected
    channel.fm.txToneEn = 0;
    channel.fm.txTone   = 0;
    return channel;
}
