#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Packages the firmware for OTA and validates everything a device will check.

Produces, from arduino-cli's raw .bin:
  firmware.bin         unsigned gzip (legacy devices, no verification)
  firmware.signed.bin  gzip + RSA/SHA-256 signature + u32 LE signature length
                       (the format of the ESP8266 core's signing.py), for
                       devices from before the tagged signature
  firmware.tagged.bin  the same layout, but the signature covers
                       SHA-256(SHA-256(gzip) || tag): the device checks it
                       against the version it asked for (TaggedHash in
                       OTAService.h), so an older release cannot pass as a
                       newer one

Why two files: eboot reads the uncompressed size from the image's last 4
bytes. Firmware with verification strips the signature before writing the
boot command; legacy firmware does not, and would read the wrong size.

Usage: package_ota.py <raw.bin> <out_dir> [--key private.key] [--pubkey public.key] [--tag vN]
Without --key, only firmware.bin is produced; without --tag, no
firmware.tagged.bin.
"""
import argparse
import gzip
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

SECTOR = 4096
# Layout eesz=1M64 + ld/eagle.flash.1m64.ota.ld (same as FlashLayout.h).
# The linker rejects IROM beyond IROM_LEN; the 400 KB up to the FS never go
# into the sketch. Updater still writes up to UPDATE_END.
UPDATE_END = 0xEB000
IROM_LEN = 0x85FF0
OTA_RESERVE = 0x64000
MAX_SKETCH = 0x87000  # 0x1010 + IROM_LEN
STOCK_IROM_LEN = 0xE9FF0
# Largest 1M64 sketch without the reserve (~530 KB today). The first OTA from
# those devices still has to fit in UPDATE_END.
LEGACY_SKETCH = 540_000
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LD_PATH = os.path.join(REPO, "ld", "eagle.flash.1m64.ota.ld")


def rounded(n: int) -> int:
    return (n + SECTOR - 1) // SECTOR * SECTOR


def fail(msg: str) -> None:
    print(f"::error::{msg}")
    sys.exit(1)


def align_gzip(gz: bytes) -> bytes:
    """Pads the gzip to a multiple of 4 bytes through the header's FNAME field.

    Updater::end() in core 3.1.2 reads the payload in 128 B blocks with
    flashRead(uint32_t*), which rejects sizes not multiple of 4, and the
    Updater ignores the error: the last block keeps bytes from the previous
    one, the hash does not match and every signed image is rejected. The last
    4 bytes (ISIZE, read by eboot) and the deflate stream do not change;
    eboot's uzlib skips FNAME.
    """
    pad = -len(gz) % 4
    if pad == 0:
        return gz
    if gz[3] != 0:
        fail("gzip with unexpected flags; cannot align it with FNAME")
    name = b"f" * ((pad if pad >= 2 else pad + 4) - 1) + b"\0"
    return gz[:3] + b"\x08" + gz[4:10] + name + gz[10:]


def device_view(payload: bytes) -> bytes:
    """What Updater::end() in core 3.1.2 actually feeds to SHA-256."""
    out = bytearray()
    buff = bytearray(128)
    for off in range(0, len(payload), 128):
        n = min(128, len(payload) - off)
        if n % 4 == 0:  # flashRead(uint32_t*) only reads aligned sizes
            buff[:n] = payload[off:off + n]
        out += buff[:n]
    return bytes(out)


def signed_message(payload: bytes, tag) -> bytes:
    """What the signature covers: the image, or SHA-256(image) + tag.

    openssl hashes the message once more, so the signed digest is
    SHA-256(SHA-256(image) || tag), as TaggedHash computes on the device.
    """
    return payload if tag is None else hashlib.sha256(payload).digest() + tag.encode()


def sign(payload: bytes, key: str, tag=None) -> bytes:
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key],
                         input=signed_message(payload, tag), capture_output=True, check=True).stdout
    return payload + sig + struct.pack("<L", len(sig))


def verify(signed: bytes, pubkey: str, tag=None) -> bytes:
    (sig_len,) = struct.unpack("<L", signed[-4:])
    payload = signed[: -4 - sig_len]
    sig = signed[-4 - sig_len: -4]
    with tempfile.TemporaryDirectory() as tmp:
        sig_path = os.path.join(tmp, "sig")
        with open(sig_path, "wb") as f:
            f.write(sig)
        r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pubkey,
                            "-signature", sig_path], input=signed_message(payload, tag),
                           capture_output=True)
    if r.returncode != 0:
        fail("signature does not match public.key: " + r.stderr.decode())
    if device_view(payload) != payload:
        fail("the device's Updater would compute another hash: payload not 4 B aligned")
    return payload


def check_gzip(gz: bytes, raw: bytes, label: str) -> None:
    if gz[:2] != b"\x1f\x8b":
        fail(f"{label}: missing gzip header")
    (isize,) = struct.unpack("<L", gz[-4:])
    if isize != len(raw):
        fail(f"{label}: ISIZE {isize} != real size {len(raw)} (eboot would copy it wrong)")
    if gzip.decompress(gz) != raw:
        fail(f"{label}: decompressed content differs from the original")


def eboot_isize(image: bytes, label: str) -> int:
    """eboot takes the last 4 bytes of a gzip as the uncompressed size."""
    if len(image) < 6 or image[:2] != b"\x1f\x8b":
        fail(f"{label}: eboot needs a gzip at the start")
    return struct.unpack("<L", image[-4:])[0]


def check_fits(image: int, running: int, raw: int, label: str) -> None:
    start = UPDATE_END - rounded(image)
    if start < rounded(running):
        fail(f"{label}: no room: running {running} B + image {image} B > {UPDATE_END} B")
    if raw > start:
        fail(f"{label}: decompressed image ({raw} B) would overwrite the source at {start:#x}")


def check_ld() -> None:
    if not os.path.isfile(LD_PATH):
        fail(f"missing linker script: {LD_PATH}")
    text = open(LD_PATH, encoding="utf-8").read()
    m = re.search(
        r"irom0_0_seg\s*:\s*org\s*=\s*(0x[0-9a-fA-F]+),\s*len\s*=\s*(0x[0-9a-fA-F]+)",
        text)
    if not m:
        fail("ld: irom0_0_seg not found")
    org, length = int(m.group(1), 16), int(m.group(2), 16)
    if org != 0x40201010:
        fail(f"ld: irom org {org:#x} != 0x40201010")
    if length != IROM_LEN:
        fail(f"ld: irom len {length:#x} != {IROM_LEN:#x} (FlashLayout.h)")
    if STOCK_IROM_LEN - length != OTA_RESERVE:
        fail("ld: OTA_RESERVE is not stock IROM - irom len")
    if not re.search(r"_FS_start\s*=\s*0x402EB000", text):
        fail("ld: _FS_start must stay 0x402EB000")


def check_map(path: str) -> None:
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"irom0_0_seg\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", text)
    if not m:
        fail(f"{path}: irom0_0_seg missing from the map")
    length = int(m.group(1), 16)
    if length != IROM_LEN:
        fail(
            f"{path}: irom len {length:#x} != {IROM_LEN:#x}; "
            "compile with build.flash_ld=eagle.flash.1m64.ota.ld")


def check_reserve(image: int, raw: int, label: str) -> None:
    if raw > MAX_SKETCH:
        fail(f"{label}: sketch {raw} B > maximum {MAX_SKETCH} B (OTA reserve)")
    if image > OTA_RESERVE:
        fail(f"{label}: image {image} B > OTA reserve {OTA_RESERVE} B")


def package_signed(gz: bytes, raw: bytes, args, name: str, tag) -> str:
    """Signs gz (over the image alone, or with tag), checks it as the device will, writes it."""
    signed = sign(gz, args.key, tag)
    check_gzip(verify(signed, args.pubkey, tag), raw, name)
    sig_len = struct.unpack("<L", signed[-4:])[0]
    if sig_len != 256:
        fail(f"RSA-2048 signature has {sig_len} bytes; expected 256")
    if eboot_isize(signed, name) != sig_len:
        fail("the last 4 bytes of the signed file are not the signature length")
    payload = signed[: -4 - sig_len]
    if eboot_isize(payload, "gzip after stripping the signature") != len(raw):
        fail("Update.end() would make eboot read the wrong size")
    check_reserve(len(signed), len(raw), name)
    check_fits(len(signed), len(raw), len(raw), "new → next")
    check_fits(len(signed), MAX_SKETCH, len(raw), "maximum → next")
    with open(os.path.join(args.out_dir, name), "wb") as f:
        f.write(signed)
    return f"| {name} | {len(signed)} B |"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out_dir")
    ap.add_argument("--key")
    ap.add_argument("--pubkey", default="public.key")
    ap.add_argument("--map", help="link map; confirms the shrunk IROM")
    ap.add_argument("--tag", help="release tag (vN); also writes firmware.tagged.bin")
    args = ap.parse_args()

    check_ld()
    if args.map:
        check_map(args.map)

    raw = open(args.raw, "rb").read()
    if raw[0] != 0xE9:
        fail("input binary is not an ESP8266 image")

    gz = align_gzip(gzip.compress(raw, compresslevel=9, mtime=0))
    if len(gz) % 4:
        fail("firmware.bin: gzip did not end up 4 B aligned")
    check_gzip(gz, raw, "firmware.bin")
    if eboot_isize(gz, "firmware.bin") != len(raw):
        fail("firmware.bin: eboot would read a size different from the firmware")
    check_reserve(len(gz), len(raw), "firmware.bin")
    check_fits(len(gz), LEGACY_SKETCH, len(raw), "legacy → new")

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "firmware.bin"), "wb") as f:
        f.write(gz)

    lines = [f"| raw | {len(raw)} B |", f"| gzip | {len(gz)} B |"]

    if args.tag is not None and not re.fullmatch(r"v[0-9]{1,9}|ci-[0-9a-f]{7}", args.tag):
        fail(f"tag {args.tag!r} is neither vN nor ci-<sha>")
    if args.key:
        lines.append(package_signed(gz, raw, args, "firmware.signed.bin", None))
        if args.tag is not None:
            lines.append(package_signed(gz, raw, args, "firmware.tagged.bin", args.tag))
    lines.append(f"| OTA reserve | {OTA_RESERVE} B |")
    lines.append(f"| maximum sketch | {MAX_SKETCH} B |")
    lines.append(f"| limit (running + OTA) | {UPDATE_END} B |")
    summary = "| Image | Size |\n|---|---|\n" + "\n".join(lines) + "\n"
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("### Firmware OTA\n\n" + summary)


if __name__ == "__main__":
    main()
