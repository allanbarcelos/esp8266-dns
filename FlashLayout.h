// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <stdint.h>

// 1M64 layout with a 400 KB OTA hole. The linker uses
// ld/eagle.flash.1m64.ota.ld (IROM 0x85ff0) and rejects a sketch in that range.
// The same numbers are in .github/scripts/package_ota.py.
struct FlashLayout {
    static constexpr uint32_t FS_START    = 0xEB000;
    static constexpr uint32_t OTA_RESERVE = 0x64000;  // 400 KiB
    static constexpr uint32_t IROM_LEN    = 0x85FF0;  // stock 0xE9FF0 - OTA_RESERVE
    static constexpr uint32_t MAX_SKETCH  = 0x87000;  // 0x1010 + IROM_LEN
};

static_assert(FlashLayout::IROM_LEN + FlashLayout::OTA_RESERVE == 0xE9FF0,
              "IROM + reserve = stock 1M64 IROM");
static_assert(FlashLayout::MAX_SKETCH == 0x1010 + FlashLayout::IROM_LEN,
              "max sketch = stub + IROM");
static_assert(FlashLayout::FS_START ==
                  FlashLayout::MAX_SKETCH + FlashLayout::OTA_RESERVE,
              "FS starts at the end of the OTA reserve");
