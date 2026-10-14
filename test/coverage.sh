#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Runs the native tests with instrumentation and requires 100% coverage
# (lines, functions and branches) of the firmware headers. Needs gcc, PlatformIO and gcovr.
set -euo pipefail
cd "$(dirname "$0")/.."

# On a clean build PlatformIO wipes the directory after the first suite,
# dropping its data. The first run only warms up the build; the
# second (with no rebuild) produces the full coverage.
pio test -e coverage
find .pio/build/coverage -name '*.gcda' -delete
pio test -e coverage

gcovr --root . --object-directory .pio/build/coverage \
      --filter '^[A-Za-z0-9]+\.h$' \
      --exclude-unreachable-branches --exclude-throw-branches \
      --print-summary --txt - \
      --fail-under-line 100 --fail-under-function 100 --fail-under-branch 100
