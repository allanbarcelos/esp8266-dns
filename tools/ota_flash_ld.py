# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Instala ld/eagle.flash.1m64.ota.ld no core ESP8266.
#
# O recipe do arduino-cli copia tools/sdk/ld/{build.flash_ld}. Sem este
# arquivo lá, --build-property build.flash_ld=eagle.flash.1m64.ota.ld
# não acha o script. O PlatformIO procura o mesmo nome no SDK.
#
# PIO: extra_scripts pre: (Import("env")).
# CLI: python tools/ota_flash_ld.py

import os
import shutil
import sys

NAME = "eagle.flash.1m64.ota.ld"


def _copy(src, dst_dir):
    if not os.path.isfile(src):
        sys.stderr.write("OTA: linker script ausente: %s\n" % src)
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
    Import("env")  # noqa: F821 — SCons, só no PlatformIO
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
            "OTA: core ESP8266 nao encontrado em .arduino15 / Arduino15\n")
        sys.exit(1)
    for d in dirs:
        _copy(src, d)
