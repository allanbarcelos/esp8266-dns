# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Removes float support for printf/scanf from the link (≈16 KB of flash).
#
# The core's builder forces "-u _printf_float -u _scanf_float"; the sketch
# neither formats nor reads floats through printf/scanf (ArduinoJson has its
# own conversion). CI does the same with --build-property build.float=.
#
# PIO: extra_scripts post: (Import("env")).
Import("env")  # noqa: F821

flags = env["LINKFLAGS"]  # noqa: F821
for sym in ("_printf_float", "_scanf_float"):
    while sym in flags:
        i = flags.index(sym)
        del flags[i - 1:i + 1]  # removes the "-u" too
