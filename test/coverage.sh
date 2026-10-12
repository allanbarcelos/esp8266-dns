#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Roda os testes nativos com instrumentação e exige 100% de cobertura
# (linhas e funções) nos headers do firmware. Requer gcc, PlatformIO e gcovr.
set -euo pipefail
cd "$(dirname "$0")/.."

# Num build do zero o PlatformIO limpa o diretório após a primeira suíte,
# descartando os dados dela. A primeira execução só aquece o build; a
# segunda (sem recompilar) produz a cobertura completa.
pio test -e coverage
find .pio/build/coverage -name '*.gcda' -delete
pio test -e coverage

gcovr --root . --object-directory .pio/build/coverage \
      --filter '^[A-Za-z]+\.h$' \
      --exclude-unreachable-branches --exclude-throw-branches \
      --print-summary --txt - \
      --fail-under-line 100 --fail-under-function 100 --fail-under-branch 100
