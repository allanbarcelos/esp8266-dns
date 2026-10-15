#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""
License and authorship header on every source file of the project.

    python3 tools/license_headers.py           adds it where missing
    python3 tools/license_headers.py --check   only checks (CI uses it); exits with an error if missing

Applies to the files git knows, tracked or new (whatever .gitignore excludes
is left out). Formats without comments (Markdown, the PEM key, images) and the
local credential examples (*.example) are left out too.
"""

import subprocess
import sys
from pathlib import Path

LINES = [
    "SPDX-License-Identifier: MIT",
    "Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot",
]

MARK = "SPDX-License-Identifier:"

# Extension (or file name) → how to comment.
LINE = {
    ".h": "// {}", ".cpp": "// {}", ".ino": "// {}", ".js": "// {}", ".mjs": "// {}",
    ".py": "# {}", ".sh": "# {}", ".yml": "# {}", ".yaml": "# {}",
    ".ini": "; {}",
}
BLOCK = {
    ".ld": ("/*", " * {}", " */"),
    ".css": ("/*", " * {}", " */"),
    ".svg": ("<!--", "  {}", "-->"),
    ".html": ("<!--", "  {}", "-->"),
}
NAMES = {".gitignore": "# {}", "requirements.txt": "# {}"}


def style(path: Path):
    if path.name in NAMES:
        return ("line", NAMES[path.name])
    if path.suffix in LINE:
        return ("line", LINE[path.suffix])
    if path.suffix in BLOCK:
        return ("block", BLOCK[path.suffix])
    return None


def header(kind, fmt) -> str:
    if kind == "line":
        return "\n".join(fmt.format(l) for l in LINES) + "\n"
    open_, middle, close = fmt
    return "\n".join([open_, *(middle.format(l) for l in LINES), close]) + "\n"


def files():
    # Tracked and also new files not added yet (honoring .gitignore): a
    # freshly created file gets the header before the commit.
    out = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True, check=True).stdout
    for name in out.splitlines():
        path = Path(name)
        if path.suffix == ".example":
            continue
        if path.exists() and style(path):
            yield path


def has_header(text: str) -> bool:
    return any(MARK in line for line in text.splitlines()[:8])


def add_header(path: Path, text: str) -> str:
    kind, fmt = style(path)
    lines = text.splitlines(keepends=True)

    # The shebang must stay on the first line
    keep = 1 if lines and lines[0].startswith("#!") else 0

    before = "".join(lines[:keep])
    after = "".join(lines[keep:])
    separator = "" if after.startswith("\n") else "\n"
    return before + header(kind, fmt) + separator + after


def main() -> int:
    check = "--check" in sys.argv[1:]
    missing = []

    for path in files():
        text = path.read_text(encoding="utf-8")
        if has_header(text):
            continue
        missing.append(path)
        if not check:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(add_header(path, text))

    if check:
        if missing:
            print("Files without the license and authorship header:")
            for path in missing:
                print(f"  {path}")
            print("\nRun: python3 tools/license_headers.py")
            return 1
        print("All files have the license and authorship header.")
        return 0

    print(f"Header added to {len(missing)} file(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
