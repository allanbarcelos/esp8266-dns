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
import re
import struct
import subprocess
import sys
import tempfile

SECTOR = 4096
# Layout eesz=1M64 + ld/eagle.flash.1m64.ota.ld (iguais a FlashLayout.h).
# O linker recusa IROM além de IROM_LEN; os 400 KB até o FS nunca entram
# no sketch. Updater ainda grava até UPDATE_END.
UPDATE_END = 0xEB000
IROM_LEN = 0x85FF0
OTA_RESERVE = 0x64000
MAX_SKETCH = 0x87000  # 0x1010 + IROM_LEN
STOCK_IROM_LEN = 0xE9FF0
# Maior sketch 1M64 sem a reserva (~530 KB atuais). O primeiro OTA a partir
# desses aparelhos ainda precisa caber em UPDATE_END.
LEGACY_SKETCH = 540_000
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LD_PATH = os.path.join(REPO, "ld", "eagle.flash.1m64.ota.ld")


def rounded(n: int) -> int:
    return (n + SECTOR - 1) // SECTOR * SECTOR


def fail(msg: str) -> None:
    print(f"::error::{msg}")
    sys.exit(1)


def align_gzip(gz: bytes) -> bytes:
    """Deixa o gzip com tamanho múltiplo de 4 via campo FNAME do cabeçalho.

    Updater::end() do core 3.1.2 lê o payload em blocos de 128 B com
    flashRead(uint32_t*), que recusa tamanho não múltiplo de 4 e o Updater
    ignora o erro: o último bloco fica com bytes do anterior, o hash não
    bate e toda imagem assinada é rejeitada. Os 4 bytes finais (ISIZE, lido
    pelo eboot) e o deflate não mudam; o uzlib do eboot pula o FNAME.
    """
    pad = -len(gz) % 4
    if pad == 0:
        return gz
    if gz[3] != 0:
        fail("gzip com flags inesperadas; não dá para alinhar com FNAME")
    name = b"f" * ((pad if pad >= 2 else pad + 4) - 1) + b"\0"
    return gz[:3] + b"\x08" + gz[4:10] + name + gz[10:]


def device_view(payload: bytes) -> bytes:
    """O que Updater::end() do core 3.1.2 realmente passa ao SHA-256."""
    out = bytearray()
    buff = bytearray(128)
    for off in range(0, len(payload), 128):
        n = min(128, len(payload) - off)
        if n % 4 == 0:  # flashRead(uint32_t*) só lê tamanhos alinhados
            buff[:n] = payload[off:off + n]
        out += buff[:n]
    return bytes(out)


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
    if device_view(payload) != payload:
        fail("o Updater do aparelho calcularia outro hash: payload não alinhado a 4 B")
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


def check_ld() -> None:
    if not os.path.isfile(LD_PATH):
        fail(f"linker script ausente: {LD_PATH}")
    text = open(LD_PATH, encoding="utf-8").read()
    m = re.search(
        r"irom0_0_seg\s*:\s*org\s*=\s*(0x[0-9a-fA-F]+),\s*len\s*=\s*(0x[0-9a-fA-F]+)",
        text)
    if not m:
        fail("ld: irom0_0_seg não encontrado")
    org, length = int(m.group(1), 16), int(m.group(2), 16)
    if org != 0x40201010:
        fail(f"ld: irom org {org:#x} != 0x40201010")
    if length != IROM_LEN:
        fail(f"ld: irom len {length:#x} != {IROM_LEN:#x} (FlashLayout.h)")
    if STOCK_IROM_LEN - length != OTA_RESERVE:
        fail("ld: OTA_RESERVE não é stock IROM - irom len")
    if not re.search(r"_FS_start\s*=\s*0x402EB000", text):
        fail("ld: _FS_start deve permanecer 0x402EB000")


def check_map(path: str) -> None:
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"irom0_0_seg\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", text)
    if not m:
        fail(f"{path}: irom0_0_seg ausente no map")
    length = int(m.group(1), 16)
    if length != IROM_LEN:
        fail(
            f"{path}: irom len {length:#x} != {IROM_LEN:#x} — "
            "compile com build.flash_ld=eagle.flash.1m64.ota.ld")


def check_reserve(image: int, raw: int, label: str) -> None:
    if raw > MAX_SKETCH:
        fail(f"{label}: sketch {raw} B > máximo {MAX_SKETCH} B (reserva OTA)")
    if image > OTA_RESERVE:
        fail(f"{label}: imagem {image} B > reserva OTA {OTA_RESERVE} B")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out_dir")
    ap.add_argument("--key")
    ap.add_argument("--pubkey", default="public.key")
    ap.add_argument("--map", help="map do link; confirma IROM encolhido")
    args = ap.parse_args()

    check_ld()
    if args.map:
        check_map(args.map)

    raw = open(args.raw, "rb").read()
    if raw[0] != 0xE9:
        fail("binário de entrada não é uma imagem ESP8266")

    gz = align_gzip(gzip.compress(raw, compresslevel=9, mtime=0))
    if len(gz) % 4:
        fail("firmware.bin: gzip não ficou alinhado a 4 B")
    check_gzip(gz, raw, "firmware.bin")
    if eboot_isize(gz, "firmware.bin") != len(raw):
        fail("firmware.bin: o eboot leria um tamanho diferente do firmware")
    check_reserve(len(gz), len(raw), "firmware.bin")
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
        check_reserve(len(signed), len(raw), "firmware.signed.bin")
        check_fits(len(signed), len(raw), len(raw), "novo → próximo")
        check_fits(len(signed), MAX_SKETCH, len(raw), "máximo → próximo")
        with open(os.path.join(args.out_dir, "firmware.signed.bin"), "wb") as f:
            f.write(signed)
        lines.append(f"| gzip assinado | {len(signed)} B |")

    lines.append(f"| reserva OTA | {OTA_RESERVE} B |")
    lines.append(f"| sketch máximo | {MAX_SKETCH} B |")
    lines.append(f"| limite (atual + OTA) | {UPDATE_END} B |")
    summary = "| Imagem | Tamanho |\n|---|---|\n" + "\n".join(lines) + "\n"
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("### Firmware OTA\n\n" + summary)


if __name__ == "__main__":
    main()
