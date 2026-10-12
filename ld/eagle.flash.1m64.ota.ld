/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot
 */

/* 1MB / FS 64KB with a hard 400 KB OTA hole the linker cannot occupy.
 *
 * Stock eagle.flash.1m64.ld gives IROM len 0xe9ff0 (up to FS at 0xEB000).
 * This script shortens IROM by OTA_RESERVE 0x64000. Overflow fails the link.
 * LittleFS / EEPROM / RF cal stay at the stock 1M64 addresses.
 *
 * irom   @0x40201010  len 0x85ff0  (end 0x40287000)
 * ota    @0x40287000  .. 0x402EB000 (400 KB)
 * spiffs @0x402EB000  .. 0x402FB000 (64 KB)
 * eeprom @0x402FB000 (4KB)
 * rfcal  @0x402FC000 (4KB)
 * wifi   @0x402FD000 (12KB)
 *
 * Keep in sync with FlashLayout.h and .github/scripts/package_ota.py.
 */

MEMORY
{
  dport0_0_seg :                        org = 0x3FF00000, len = 0x10
  dram0_0_seg :                         org = 0x3FFE8000, len = 0x14000
  irom0_0_seg :                         org = 0x40201010, len = 0x85ff0
}

PROVIDE ( _FS_start = 0x402EB000 );
PROVIDE ( _FS_end = 0x402FB000 );
PROVIDE ( _FS_page = 0x100 );
PROVIDE ( _FS_block = 0x1000 );
PROVIDE ( _EEPROM_start = 0x402fb000 );
/* The following symbols are DEPRECATED and will be REMOVED in a future release */
PROVIDE ( _SPIFFS_start = 0x402EB000 );
PROVIDE ( _SPIFFS_end = 0x402FB000 );
PROVIDE ( _SPIFFS_page = 0x100 );
PROVIDE ( _SPIFFS_block = 0x1000 );

INCLUDE "local.eagle.app.v6.common.ld"
