# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Adds --coverage to the link step of the "coverage" env (libgcov).
Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
env.Append(LINKFLAGS=["--coverage"])  # noqa: F821
