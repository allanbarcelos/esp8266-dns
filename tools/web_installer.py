#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Monta o site do instalador web (ESP Web Tools) para o GitHub Pages.

    python3 tools/web_installer.py <firmware> <versão> <site_dir>

<firmware> é o .bin cru do arduino-cli ou o firmware.bin do release (gzip do
cru, sem assinatura): o gzip é descomprimido aqui, porque a gravação pela
serial escreve a imagem como está a partir do endereço 0. O
firmware.signed.bin não serve (assinatura depois do gzip).

O site recebe a página (install/), o firmware.bin cru e o manifest.json.
Antes, confere que a imagem é do ESP8266, cabe no sketch máximo do layout
(FlashLayout.h) e traz a versão informada, para o manifesto não anunciar
uma versão e gravar outra.
"""
import gzip
import json
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PAGE = REPO / "install"

IMAGE_MAGIC = 0xE9  # primeiro byte de uma imagem de app do ESP8266
MIN_IMAGE = 0x1000  # eboot + cabeçalho: menos que isso não é firmware
VERSION_RE = re.compile(r"^(v[0-9]+|ci-[0-9a-f]{7}|dev)$")


def fail(msg: str) -> None:
    print(f"ERRO: {msg}", file=sys.stderr)
    sys.exit(1)


def max_sketch() -> int:
    text = (REPO / "FlashLayout.h").read_text(encoding="utf-8")
    m = re.search(r"MAX_SKETCH\s*=\s*(0x[0-9A-Fa-f]+)", text)
    if not m:
        fail("MAX_SKETCH não encontrado em FlashLayout.h")
    return int(m.group(1), 16)


def load_image(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:2] == b"\x1f\x8b":
        try:
            data = gzip.decompress(data)
        except (OSError, EOFError) as e:  # assinatura depois do gzip, arquivo cortado
            fail(f"{path.name}: gzip inválido ({e}); use firmware.bin, não o assinado")
    if len(data) < MIN_IMAGE or data[0] != IMAGE_MAGIC:
        fail(f"{path.name}: não é uma imagem de firmware do ESP8266")
    limit = max_sketch()
    if len(data) > limit:
        fail(f"{path.name}: {len(data)} bytes, acima do sketch máximo ({limit})")
    return data


def build(firmware: Path, version: str, site: Path) -> None:
    if not VERSION_RE.match(version):
        fail(f"versão inválida: {version!r}")
    image = load_image(firmware)
    # firmware_version fica na imagem como string C
    if version.encode() + b"\0" not in image:
        fail(f"a imagem não traz a versão {version}")

    if site.exists():
        shutil.rmtree(site)
    shutil.copytree(PAGE, site)
    (site / "firmware.bin").write_bytes(image)
    manifest = {
        "name": "ESP8266 IoT",
        "version": version,
        # Instalação nova pergunta se apaga a flash (inclusive a configuração)
        "new_install_prompt_erase": True,
        # O firmware não fala Improv: o Wi-Fi é configurado pelo AP ESP_Config
        "new_install_improv_wait_time": 0,
        "builds": [{
            "chipFamily": "ESP8266",
            "parts": [{"path": "firmware.bin", "offset": 0}],
        }],
    }
    (site / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Instalador web: {version}, {len(image)} bytes em {site}")


def main() -> None:
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        sys.exit(2)
    build(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))


if __name__ == "__main__":
    main()
