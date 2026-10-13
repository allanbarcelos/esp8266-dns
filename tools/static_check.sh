#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Análise estática dos headers do firmware (cppcheck). O CI roda no job
# check; qualquer achado quebra. Requer cppcheck 2.13 ou mais novo.
#
# Fora da conta, por serem falso positivo ou estilo que o projeto não segue:
#   badBitmaskCheck     doc["x"] | 0 é o valor padrão do ArduinoJson, não máscara
#   unusedStructMember  header analisado sozinho não vê quem usa o campo
#   cstyleCast          o projeto usa cast de C em tipos simples
#   useStlAlgorithm     laço explícito é mais claro que std::find_if aqui
#   test/stubs          os stubs só imitam o core do Arduino
set -euo pipefail
cd "$(dirname "$0")/.."

cppcheck --language=c++ --std=c++17 \
    --enable=warning,performance,portability,style --check-level=exhaustive \
    --inline-suppr --error-exitcode=1 --quiet \
    -I test/stubs -I . -Dfirmware_version='"dev"' \
    --suppress=missingIncludeSystem --suppress=missingInclude \
    --suppress=badBitmaskCheck --suppress=unusedStructMember \
    --suppress=cstyleCast --suppress=useStlAlgorithm \
    --suppress='*:test/stubs/*' \
    --template='{file}:{line}: {severity} [{id}] {message}' \
    ./*.h
echo "cppcheck: nenhum achado"
