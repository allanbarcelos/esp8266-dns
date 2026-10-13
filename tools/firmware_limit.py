# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Limite de tamanho do firmware: nenhuma mudança passa de 500.000 bytes.

    python3 tools/firmware_limit.py build/firmware.raw.bin   (o CI usa)

No PlatformIO entra como extra_scripts post: e confere o firmware.bin a
cada build local, para a mudança falhar antes do push.
"""
import os
import sys

LIMIT = 500_000


def check(path):
    size = os.path.getsize(path)
    print(f"Firmware: {size} de {LIMIT} bytes (folga {LIMIT - size})")
    if size > LIMIT:
        print(f"ERRO: o firmware passou do limite de {LIMIT} bytes em {size - LIMIT}")
        return False
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("uso: firmware_limit.py <firmware.bin>")
    sys.exit(0 if check(sys.argv[1]) else 1)
else:
    Import("env")  # noqa: F821

    def _after_bin(target, source, env):
        if not check(str(target[0])):
            os.remove(str(target[0]))  # senão o próximo build o dá por pronto
            env.Exit(1)

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _after_bin)  # noqa: F821
