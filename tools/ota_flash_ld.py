# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Installs ld/eagle.flash.1m64.ota.ld into the ESP8266 core.
#
# The arduino-cli recipe copies tools/sdk/ld/{build.flash_ld}. Without this
# file there, --build-property build.flash_ld=eagle.flash.1m64.ota.ld does
# not find the script. PlatformIO looks for the same name in the SDK.
#
# PIO: extra_scripts pre: (Import("env")).
# CLI: python tools/ota_flash_ld.py

import os
import shutil
import sys

NAME = "eagle.flash.1m64.ota.ld"


def _copy(src, dst_dir):
    if not os.path.isfile(src):
        sys.stderr.write("OTA: missing linker script: %s\n" % src)
        sys.exit(1)
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, NAME)
    shutil.copy2(src, dst)
    print("OTA: linker %s -> %s" % (src, dst))
    return dst


def _arduino_ld_dirs():
    homes = [
        os.path.expanduser("~/.arduino15"),
        os.path.expanduser("~/Arduino15"),
    ]
    local = os.environ.get("LOCALAPPDATA")
    if local:
        homes.append(os.path.join(local, "Arduino15"))
    dirs = []
    seen = set()
    for home in homes:
        base = os.path.join(home, "packages", "esp8266", "hardware", "esp8266")
        if not os.path.isdir(base):
            continue
        for ver in os.listdir(base):
            ld = os.path.abspath(os.path.join(base, ver, "tools", "sdk", "ld"))
            if os.path.isdir(ld) and ld not in seen:
                seen.add(ld)
                dirs.append(ld)
    return dirs


try:
    Import("env")  # noqa: F821 (SCons, PlatformIO only)
    project = env.subst("$PROJECT_DIR")  # noqa: F821
    src = os.path.join(project, "ld", NAME)
    fw = env.PioPlatform().get_package_dir("framework-arduinoespressif8266")  # noqa: F821
    _copy(src, os.path.join(fw, "tools", "sdk", "ld"))
except NameError:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src = os.path.join(root, "ld", NAME)
    dirs = _arduino_ld_dirs()
    if not dirs:
        sys.stderr.write(
            "OTA: ESP8266 core not found in .arduino15 / Arduino15\n")
        sys.exit(1)
    for d in dirs:
        _copy(src, d)
