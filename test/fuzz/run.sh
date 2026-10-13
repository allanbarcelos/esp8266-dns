#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Fuzzing dos interpretadores de entrada externa: pacotes MQTT do broker,
# texto de comando, resposta do Cloudflare, fluxo do ntfy e config.json.
# libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer, sobre os mesmos
# stubs dos testes. Um achado para o script e deixa o caso em .pio/fuzz/.
#
# Uso: test/fuzz/run.sh [segundos por alvo]   (requer clang e PlatformIO)
set -euo pipefail
cd "$(dirname "$0")/../.."

SECS="${1:-30}"
CXX="${CXX:-clang++}"
AJ=.pio/libdeps/native/ArduinoJson/src
[ -d "$AJ" ] || pio pkg install -e native > /dev/null
OUT=.pio/fuzz
mkdir -p "$OUT"

for src in test/fuzz/fuzz_*.cpp; do
    name="$(basename "$src" .cpp)"
    "$CXX" -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        -I test/stubs -I . -isystem "$AJ" -Dfirmware_version='"fuzz"' \
        -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_PROGMEM=0 \
        -Wl,--wrap=malloc "$src" -o "$OUT/$name"
    mkdir -p "$OUT/corpus/$name"
    echo "── $name (${SECS} s)"
    # O primeiro diretório recebe o que o fuzzer descobre; o segundo são as
    # sementes do repositório, só leitura
    "$OUT/$name" -max_total_time="$SECS" -max_len=4096 -artifact_prefix="$OUT/$name-" \
        -print_final_stats=1 "$OUT/corpus/$name" "test/fuzz/corpus/$name" 2>&1 \
        | grep -E "^(stat::number_of_executed_units|stat::new_units_added|#[0-9]+ +DONE)|ERROR|SUMMARY|runtime error"
done
echo "fuzzing: nenhum achado"
