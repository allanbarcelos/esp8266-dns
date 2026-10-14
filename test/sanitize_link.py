# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Brings the sanitizers to the link step of the "sanitize" env (libasan, libubsan).
Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
env.Append(LINKFLAGS=["-fsanitize=address,undefined"])  # noqa: F821
