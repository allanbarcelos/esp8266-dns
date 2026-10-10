#!/usr/bin/env python3
"""Empacota o firmware para OTA e valida tudo o que um aparelho vai checar.

Gera, a partir do .bin cru do arduino-cli:
  firmware.bin         gzip sem assinatura (aparelhos legados, sem verificação)
  firmware.signed.bin  gzip + assinatura RSA/SHA-256 + u32 LE do tamanho da
                       assinatura (formato do signing.py do core ESP8266)

Por que dois arquivos: o eboot lê o tamanho descomprimido nos 4 últimos bytes
da imagem. Firmware com verificação remove a assinatura antes de gravar o
comando de boot; firmware legado não, e leria o tamanho errado.

Uso: package_ota.py <raw.bin> <out_dir> [--key private.key] [--pubkey public.key]
Sem --key, gera apenas firmware.bin (builds de PR).
"""
import argparse
import gzip
import os
import struct
import subprocess
import sys
import tempfile

SECTOR = 4096
# Layout eesz=1M64: o sistema de arquivos começa em 0xEB000, logo
# sketch atual + imagem OTA precisam caber em 0xEB000 bytes (Updater::begin).
UPDATE_END = 0xEB000
# Maior sketch legado conhecido (v82) — precisa conseguir instalar o novo.
LEGACY_SKETCH = 500_000


def rounded(n: int) -> int:
    return (n + SECTOR - 1) // SECTOR * SECTOR


def fail(msg: str) -> None:
    print(f"::error::{msg}")
    sys.exit(1)


def sign(payload: bytes, key: str) -> bytes:
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key],
                         input=payload, capture_output=True, check=True).stdout
    return payload + sig + struct.pack("<L", len(sig))


def verify(signed: bytes, pubkey: str) -> bytes:
    (sig_len,) = struct.unpack("<L", signed[-4:])
    payload = signed[: -4 - sig_len]
    sig = signed[-4 - sig_len: -4]
    with tempfile.TemporaryDirectory() as tmp:
        sig_path = os.path.join(tmp, "sig")
        with open(sig_path, "wb") as f:
            f.write(sig)
        r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pubkey,
                            "-signature", sig_path], input=payload, capture_output=True)
    if r.returncode != 0:
        fail("assinatura não confere com public.key: " + r.stderr.decode())
    return payload


def check_gzip(gz: bytes, raw: bytes, label: str) -> None:
    if gz[:2] != b"\x1f\x8b":
        fail(f"{label}: cabeçalho gzip ausente")
    (isize,) = struct.unpack("<L", gz[-4:])
    if isize != len(raw):
        fail(f"{label}: ISIZE {isize} != tamanho real {len(raw)} (eboot copiaria errado)")
    if gzip.decompress(gz) != raw:
        fail(f"{label}: conteúdo descomprimido difere do original")


def eboot_isize(image: bytes, label: str) -> int:
    """O eboot trata os 4 últimos bytes de um gzip como o tamanho descomprimido."""
    if len(image) < 6 or image[:2] != b"\x1f\x8b":
        fail(f"{label}: o eboot exige gzip no início")
    return struct.unpack("<L", image[-4:])[0]


def check_fits(image: int, running: int, raw: int, label: str) -> None:
    start = UPDATE_END - rounded(image)
    if start < rounded(running):
        fail(f"{label}: sem espaço — atual {running} B + imagem {image} B > {UPDATE_END} B")
    if raw > start:
        fail(f"{label}: imagem descomprimida ({raw} B) sobrescreveria a origem em {start:#x}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out_dir")
    ap.add_argument("--key")
    ap.add_argument("--pubkey", default="public.key")
    args = ap.parse_args()

    raw = open(args.raw, "rb").read()
    if raw[0] != 0xE9:
        fail("binário de entrada não é uma imagem ESP8266")

    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    check_gzip(gz, raw, "firmware.bin")
    if eboot_isize(gz, "firmware.bin") != len(raw):
        fail("firmware.bin: o eboot leria um tamanho diferente do firmware")
    check_fits(len(gz), LEGACY_SKETCH, len(raw), "legado → novo")

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "firmware.bin"), "wb") as f:
        f.write(gz)

    lines = [f"| raw | {len(raw)} B |", f"| gzip | {len(gz)} B |"]

    if args.key:
        signed = sign(gz, args.key)
        check_gzip(verify(signed, args.pubkey), raw, "firmware.signed.bin")
        sig_len = struct.unpack("<L", signed[-4:])[0]
        if sig_len != 256:
            fail(f"assinatura RSA-2048 tem {sig_len} bytes; o esperado é 256")
        if eboot_isize(signed, "firmware.signed.bin") != sig_len:
            fail("os 4 últimos bytes do arquivo assinado não são o tamanho da assinatura")
        payload = signed[: -4 - sig_len]
        if eboot_isize(payload, "gzip após remover a assinatura") != len(raw):
            fail("Update.end() deixaria o eboot ler o tamanho errado")
        check_fits(len(signed), len(raw), len(raw), "novo → próximo")
        with open(os.path.join(args.out_dir, "firmware.signed.bin"), "wb") as f:
            f.write(signed)
        lines.append(f"| gzip assinado | {len(signed)} B |")

    lines.append(f"| limite (atual + OTA) | {UPDATE_END} B |")
    summary = "| Imagem | Tamanho |\n|---|---|\n" + "\n".join(lines) + "\n"
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("### Firmware OTA\n\n" + summary)


if __name__ == "__main__":
    main()
