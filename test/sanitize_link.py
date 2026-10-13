# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Leva os sanitizers à etapa de link do env "sanitize" (libasan, libubsan).
Import("env")  # noqa: F821  (injetado pelo PlatformIO/SCons)
env.Append(LINKFLAGS=["-fsanitize=address,undefined"])  # noqa: F821
