#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Builds the web installer site (ESP Web Tools) for GitHub Pages.

    python3 tools/web_installer.py <firmware> <version> <site_dir>

<firmware> is the raw .bin from arduino-cli or the release's firmware.bin
(gzip of the raw image, unsigned): the gzip is decompressed here, because
flashing over serial writes the image as is starting at address 0. The
firmware.signed.bin does not work (signature after the gzip).

The site gets the page (install/), the raw firmware.bin and manifest.json.
Before that, it checks that the image is an ESP8266 one, fits the layout's
maximum sketch (FlashLayout.h) and carries the given version, so the manifest
never announces one version and flashes another.
"""
import gzip
import json
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PAGE = REPO / "install"

IMAGE_MAGIC = 0xE9  # first byte of an ESP8266 app image
MIN_IMAGE = 0x1000  # eboot + header: anything smaller is not firmware
VERSION_RE = re.compile(r"^(v[0-9]+|ci-[0-9a-f]{7}|dev)$")


def fail(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def max_sketch() -> int:
    text = (REPO / "FlashLayout.h").read_text(encoding="utf-8")
    m = re.search(r"MAX_SKETCH\s*=\s*(0x[0-9A-Fa-f]+)", text)
    if not m:
        fail("MAX_SKETCH not found in FlashLayout.h")
    return int(m.group(1), 16)


def load_image(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:2] == b"\x1f\x8b":
        try:
            data = gzip.decompress(data)
        except (OSError, EOFError) as e:  # signature after the gzip, truncated file
            fail(f"{path.name}: invalid gzip ({e}); use firmware.bin, not the signed one")
    if len(data) < MIN_IMAGE or data[0] != IMAGE_MAGIC:
        fail(f"{path.name}: not an ESP8266 firmware image")
    limit = max_sketch()
    if len(data) > limit:
        fail(f"{path.name}: {len(data)} bytes, above the maximum sketch ({limit})")
    return data


def build(firmware: Path, version: str, site: Path) -> None:
    if not VERSION_RE.match(version):
        fail(f"invalid version: {version!r}")
    image = load_image(firmware)
    # firmware_version is in the image as a C string
    if version.encode() + b"\0" not in image:
        fail(f"the image does not carry version {version}")

    if site.exists():
        shutil.rmtree(site)
    shutil.copytree(PAGE, site)
    (site / "firmware.bin").write_bytes(image)
    manifest = {
        "name": "ESP8266 IoT",
        "version": version,
        # A new install asks whether to erase the flash (configuration included)
        "new_install_prompt_erase": True,
        # The firmware does not speak Improv: Wi-Fi is set up through the ESP_Config AP
        "new_install_improv_wait_time": 0,
        "builds": [{
            "chipFamily": "ESP8266",
            "parts": [{"path": "firmware.bin", "offset": 0}],
        }],
    }
    (site / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Web installer: {version}, {len(image)} bytes in {site}")


def main() -> None:
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        sys.exit(2)
    build(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))


if __name__ == "__main__":
    main()
