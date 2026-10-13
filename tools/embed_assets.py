# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Gera Assets.h com as imagens do painel embutidas no firmware.

No modo AP não há internet, então o painel não pode buscar imagens fora.
- docs/esp-01.jpg: recortada nas bordas brancas, 264 px, WebP qualidade 40
  (exibida a 220 px; a sobra mantém a nitidez em telas de alta densidade).
- docs/esp01-pinout.svg: espaços entre tags removidos e gzip (servido com
  Content-Encoding: gzip).

Uso: python tools/embed_assets.py   (requer Pillow com suporte a WebP)
O Assets.h gerado vai para o repositório: o build não depende do Pillow.
"""
import gzip
import io
import os
import re

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PHOTO_WIDTH = 264
PHOTO_QUALITY = 40


def photo():
    im = Image.open(os.path.join(ROOT, "docs", "esp-01.jpg")).convert("RGB")
    # Recorta o fundo branco: tudo acima de 245 em cinza conta como borda
    box = Image.eval(im.convert("L"), lambda v: 255 if v < 245 else 0).getbbox()
    im = im.crop(box)
    height = round(im.height * PHOTO_WIDTH / im.width)
    im = im.resize((PHOTO_WIDTH, height), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, "WEBP", quality=PHOTO_QUALITY, method=6)
    return out.getvalue(), im.size


def pinout():
    with open(os.path.join(ROOT, "docs", "esp01-pinout.svg"), "rb") as f:
        svg = re.sub(rb"<!--.*?-->", b"", f.read(), flags=re.S)  # cabeçalho de licença
        svg = re.sub(rb">\s+<", b"><", svg).strip()
    return gzip.compress(svg, 9, mtime=0)


def array(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16])
            for i in range(0, len(data), 16)]
    body = ",\n    ".join(rows)
    return f"static const uint8_t {name}[] PROGMEM = {{\n    {body}\n}};\n"


def main():
    jpg, (w, h) = photo()
    svg = pinout()
    text = (
        "// SPDX-License-Identifier: MIT\n"
        "// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: "
        "https://github.com/allanbarcelos/esp8266-iot\n\n"
        "#pragma once\n"
        "// Gerado por tools/embed_assets.py a partir de docs/. Não edite.\n"
        "#include <Arduino.h>\n\n"
        f"// Foto do ESP-01, WebP {w}x{h} ({len(jpg)} bytes)\n"
        + array("ASSET_PHOTO", jpg) +
        f"\n// Pinos do ESP-01, SVG com gzip ({len(svg)} bytes)\n"
        + array("ASSET_PINOUT_GZ", svg)
    )
    with open(os.path.join(ROOT, "Assets.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"Assets.h: foto {len(jpg)} B, pinos {len(svg)} B")


if __name__ == "__main__":
    main()
