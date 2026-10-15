#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Static analysis of the firmware headers (cppcheck). CI runs it in the check
# job; any finding fails. Needs cppcheck 2.13 or newer.
#
# Left out, as false positives or a style the project does not follow:
#   unusedStructMember  a header analyzed alone does not see who uses the field
#   cstyleCast          the project uses C casts on simple types
#   useStlAlgorithm     an explicit loop is clearer than std::find_if here
#   test/stubs          the stubs only imitate the Arduino core
set -euo pipefail
cd "$(dirname "$0")/.."

cppcheck --language=c++ --std=c++17 \
    --enable=warning,performance,portability,style --check-level=exhaustive \
    --inline-suppr --error-exitcode=1 --quiet \
    -I test/stubs -I . -Dfirmware_version='"dev"' \
    --suppress=missingIncludeSystem --suppress=missingInclude \
    --suppress=unusedStructMember \
    --suppress=cstyleCast --suppress=useStlAlgorithm \
    --suppress='*:test/stubs/*' \
    --template='{file}:{line}: {severity} [{id}] {message}' \
    ./*.h
echo "cppcheck: no findings"
