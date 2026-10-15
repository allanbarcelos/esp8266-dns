# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

# Embeds public.key in the ESP8266 firmware before the core compiles.
#
# PlatformIO does not run arduino-cli's recipe.hooks. Without this header,
# the core's Updater_Signing.h defines ARDUINO_SIGNING 0, Update.end() does
# not strip the signature and eboot reads the signature length as if it were
# the uncompressed size.
#
# It must be both a pre: and a post: extra_script. pre: puts the -I in place
# before the builder clones the Wi-Fi library environment. post: hooks the
# check to the firmware.elf already built. Both are required.
Import("env")

import os
import subprocess
import sys

project = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
if not os.path.isabs(build_dir):
    build_dir = os.path.join(project, build_dir)
out_dir = os.path.abspath(os.path.join(build_dir, "signing"))
header = os.path.join(out_dir, "Updater_Signing.h")
pubkey = os.path.join(project, "public.key")
framework = env.PioPlatform().get_package_dir("framework-arduinoespressif8266")
signing_py = os.path.join(framework, "tools", "signing.py")

os.makedirs(out_dir, exist_ok=True)
if not os.path.isfile(pubkey):
    sys.stderr.write("public.key is missing; OTA cannot verify the signature\n")
    env.Exit(1)
if not os.path.isfile(signing_py):
    sys.stderr.write("signing.py from the ESP8266 core not found\n")
    env.Exit(1)

subprocess.check_call(
    [sys.executable, signing_py, "--mode", "header",
     "--publickey", pubkey, "--out", header]
)

with open(header, "r", encoding="utf-8") as handle:
    generated = handle.read()
if "#define ARDUINO_SIGNING 1" not in generated or "signing_pubkey" not in generated:
    sys.stderr.write("Updater_Signing.h does not enable signing\n")
    env.Exit(1)

# pre: still sees PROGNAME "program". The platform builder switches it to
# "firmware" before the post: scripts.
late = env.get("PROGNAME", "program") not in ("", "program")

if not env.get("OTA_PUBKEY_PATH"):
    if late:
        sys.stderr.write(
            "OTA: tools/embed_pubkey.py must be pre: before the core\n")
        env.Exit(1)
    # First in the -I list. cores/esp8266 also has an Updater_Signing.h that
    # turns signing off; it must not win the search.
    env.Prepend(CPPPATH=[out_dir])
    env.Replace(OTA_PUBKEY_PATH=out_dir)


def _require_verifier(source, target, env):
    del source
    elf = str(target[0])
    if not os.path.isfile(elf):
        sys.stderr.write("OTA: ELF missing, signing not confirmed\n")
        env.Exit(1)
    nm = os.path.join(
        env.PioPlatform().get_package_dir("toolchain-xtensa"),
        "bin", "xtensa-lx106-elf-nm")
    if os.name == "nt":
        nm += ".exe"
    if not os.path.isfile(nm):
        sys.stderr.write("OTA: xtensa-lx106-elf-nm not found\n")
        env.Exit(1)
    symbols = subprocess.check_output(
        [nm, "-C", elf], text=True, errors="replace")
    missing = [
        name for name in ("updaterSigningVerifier", "updaterSigningHash")
        if name not in symbols
    ]
    if missing:
        sys.stderr.write(
            "OTA: firmware without " + ", ".join(missing)
            + ". The key did not get into the Updater.\n")
        env.Exit(1)
    pem = open(pubkey, "rb").read()
    blob = open(elf, "rb").read()
    body = b"\n".join(
        line for line in pem.splitlines()
        if line and not line.startswith(b"-----"))
    if pem not in blob and body not in blob:
        sys.stderr.write("OTA: public.key is not inside the firmware\n")
        env.Exit(1)
    # The sketch and the Updater must agree. Without this literal the device
    # would download the plain gzip and Update.end() would reject the image.
    if b"firmware.tagged.bin" not in blob:
        sys.stderr.write(
            "OTA: the sketch does not ask for firmware.tagged.bin; "
            "ARDUINO_SIGNING did not reach OTAService\n")
        env.Exit(1)
    print("OTA: updaterSigningVerifier, public.key and firmware.tagged.bin are in the firmware")


if late and not env.get("OTA_SIGNING_CHECK"):
    env.Replace(OTA_SIGNING_CHECK=True)
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _require_verifier)
