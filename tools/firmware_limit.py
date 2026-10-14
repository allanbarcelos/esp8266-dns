# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Firmware size limit: no change goes past 500,000 bytes.

    python3 tools/firmware_limit.py build/firmware.raw.bin   (CI uses it)

In PlatformIO it runs as a post: extra_script and checks firmware.bin on
every local build, so the change fails before the push.
"""
import os
import sys

LIMIT = 500_000


def check(path):
    size = os.path.getsize(path)
    print(f"Firmware: {size} of {LIMIT} bytes ({LIMIT - size} to spare)")
    if size > LIMIT:
        print(f"ERROR: the firmware is {size - LIMIT} bytes over the {LIMIT}-byte limit")
        return False
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: firmware_limit.py <firmware.bin>")
    sys.exit(0 if check(sys.argv[1]) else 1)
else:
    Import("env")  # noqa: F821

    def _after_bin(target, source, env):
        if not check(str(target[0])):
            os.remove(str(target[0]))  # otherwise the next build takes it as done
            env.Exit(1)

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _after_bin)  # noqa: F821
