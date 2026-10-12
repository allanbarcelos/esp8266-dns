# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Adiciona --coverage na etapa de link do env "coverage" (libgcov).
Import("env")  # noqa: F821  (injetado pelo PlatformIO/SCons)
env.Append(LINKFLAGS=["--coverage"])  # noqa: F821
