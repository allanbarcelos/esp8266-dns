#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""
Cabeçalho de licença e autoria em todo arquivo de código do projeto.

    python3 tools/license_headers.py           acrescenta onde falta
    python3 tools/license_headers.py --check   só confere (o CI usa); sai com erro se faltar

Vale para os arquivos do git, versionados ou novos (o que o .gitignore exclui
fica de fora). Ficam de fora os formatos sem comentário (Markdown, a chave
PEM, imagens) e os exemplos locais de credenciais (*.example).
"""

import subprocess
import sys
from pathlib import Path

LINHAS = [
    "SPDX-License-Identifier: MIT",
    "Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot",
]

MARCA = "SPDX-License-Identifier:"

# Extensão (ou nome de arquivo) → como comentar.
LINHA = {
    ".h": "// {}", ".cpp": "// {}", ".ino": "// {}",
    ".py": "# {}", ".sh": "# {}", ".yml": "# {}", ".yaml": "# {}",
    ".ini": "; {}",
}
BLOCO = {
    ".ld": ("/*", " * {}", " */"),
    ".svg": ("<!--", "  {}", "-->"),
}
NOMES = {".gitignore": "# {}", "requirements.txt": "# {}"}


def estilo(caminho: Path):
    if caminho.name in NOMES:
        return ("linha", NOMES[caminho.name])
    if caminho.suffix in LINHA:
        return ("linha", LINHA[caminho.suffix])
    if caminho.suffix in BLOCO:
        return ("bloco", BLOCO[caminho.suffix])
    return None


def cabecalho(tipo, formato) -> str:
    if tipo == "linha":
        return "\n".join(formato.format(l) for l in LINHAS) + "\n"
    abre, meio, fecha = formato
    return "\n".join([abre, *(meio.format(l) for l in LINHAS), fecha]) + "\n"


def arquivos():
    # Versionados e também os novos ainda não adicionados (respeitando o
    # .gitignore): um arquivo recém-criado recebe o cabeçalho antes do commit.
    saida = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True, check=True).stdout
    for nome in saida.splitlines():
        caminho = Path(nome)
        if caminho.suffix == ".example":
            continue
        if caminho.exists() and estilo(caminho):
            yield caminho


def tem_cabecalho(texto: str) -> bool:
    return any(MARCA in linha for linha in texto.splitlines()[:8])


def acrescentar(caminho: Path, texto: str) -> str:
    tipo, formato = estilo(caminho)
    linhas = texto.splitlines(keepends=True)

    # O shebang precisa continuar na primeira linha
    preservar = 1 if linhas and linhas[0].startswith("#!") else 0

    antes = "".join(linhas[:preservar])
    depois = "".join(linhas[preservar:])
    separador = "" if depois.startswith("\n") else "\n"
    return antes + cabecalho(tipo, formato) + separador + depois


def main() -> int:
    conferir = "--check" in sys.argv[1:]
    faltando = []

    for caminho in arquivos():
        texto = caminho.read_text(encoding="utf-8")
        if tem_cabecalho(texto):
            continue
        faltando.append(caminho)
        if not conferir:
            with open(caminho, "w", encoding="utf-8", newline="") as f:
                f.write(acrescentar(caminho, texto))

    if conferir:
        if faltando:
            print("Arquivos sem o cabeçalho de licença e autoria:")
            for caminho in faltando:
                print(f"  {caminho}")
            print("\nRode: python3 tools/license_headers.py")
            return 1
        print("Todos os arquivos têm o cabeçalho de licença e autoria.")
        return 0

    print(f"Cabeçalho acrescentado em {len(faltando)} arquivo(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
