# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

"""Generates WebAssets.h with the panel CSS and JavaScript, minified and gzipped,
and the panel text in every language from web/i18n.tsv.

The device serves each file as is, with Content-Encoding: gzip: the browser
decompresses it and caches it. The URL carries part of the content's SHA-256
(?v=), so a new version never uses the old cache.

Panel text: the firmware sends a marker (` plus two base-62 characters, the
row in web/i18n.tsv) in place of each text; web/i18n.js carries the text of
every language in one matrix and writes the page in the chosen language. So
the text goes into the flash once, gzipped, and the languages share the
words they have in common. Generated from the table:
  I18n.h           L_<key> macros with the markers, for the C++
  test/i18n_en.h   the English text, for the native tests to read the pages
  web/iot.js       L('English text') becomes L(row)

    python tools/embed_web.py           regenerates the files above from web/
    python tools/embed_web.py --min-iot FILE   writes web/iot.js minified, with
                                        its L('English') kept: web/iot.test.mjs runs
                                        on it too (IOT_JS=FILE)
    python tools/embed_web.py --check   only checks (CI uses it); exits with an
                                        error if a generated file does not match

The check compares the decompressed content, not the gzip bytes: another
compressor (zopfli when installed, otherwise zlib) gives other bytes without
being wrong.
"""
import gzip
import hashlib
import os
import re
import sys

try:
    import zopfli.gzip as _zopfli
except ImportError:  # optional: only makes the gzip smaller
    _zopfli = None

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "WebAssets.h")
I18N_H = os.path.join(ROOT, "I18n.h")
TEST_EN = os.path.join(ROOT, "test", "i18n_en.h")
TABLE = os.path.join(ROOT, "web", "i18n.tsv")

# (file in web/, name in C++)
FILES = (("panel.css", "WEB_CSS"), ("iot.js", "WEB_IOT"), ("i18n.js", "WEB_I18N"))

LANGS = ("en", "pt", "es", "fr")
B62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
RESERVED = "`|~^\t\""   # marker, matrix separators and the JS string quote

LICENSE = (
    "// SPDX-License-Identifier: MIT\n"
    "// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: "
    "https://github.com/allanbarcelos/esp8266-iot\n\n"
)


def load_table():
    """Rows of web/i18n.tsv: (key, [en, pt, es, fr]), an empty column = en."""
    rows, keys, english = [], set(), set()
    with open(TABLE, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            key, *cols = line.split("\t")
            cols += [""] * (len(LANGS) - len(cols))
            if len(cols) != len(LANGS) or not re.fullmatch(r"[A-Z][A-Z0-9_]*", key) or not cols[0]:
                raise SystemExit(f"web/i18n.tsv:{n}: expected key, en, pt, es, fr")
            if key in keys or cols[0] in english:
                raise SystemExit(f"web/i18n.tsv:{n}: repeated key or English text")
            for c in cols:
                if any(ch in c for ch in RESERVED) or c != c.strip():
                    raise SystemExit(f"web/i18n.tsv:{n}: {c!r} has spaces at the ends or one of {RESERVED!r}")
            keys.add(key)
            english.add(cols[0])
            rows.append((key, [c or cols[0] for c in cols]))
    if len(rows) > len(B62) ** 2:
        raise SystemExit("web/i18n.tsv: more rows than two-character markers")
    return rows


def marker(n):
    return "`" + B62[n // 62] + B62[n % 62]


def similar_first(rows):
    """Row order for the matrix: each row is followed by the one that shares
    the most 4-letter pieces with it (in any language). Deflate then finds
    the repeats closer, with shorter distance codes: ~4 % smaller than the
    table order. The marker is the position in this order."""
    def grams(cols):
        t = " ".join(cols).lower()
        return {t[i:i + 4] for i in range(len(t) - 3)}
    left = [(r, grams(r[1])) for r in rows]
    cur = left.pop(0)
    out = [cur[0]]
    while left:
        k = max(range(len(left)),
                key=lambda i: len(cur[1] & left[i][1]) / (1 + len(left[i][1])) ** 0.5)
        cur = left.pop(k)
        out.append(cur[0])
    return out


def matrix(rows):
    """One column per language (en|pt|es|fr), rows joined by ~. A cell equal
    to English is empty and one equal to the language before it is ^:
    repeated text costs one byte or none. Column by column, each language
    sits next to itself: ~1 % smaller than row by row."""
    cols = []
    for j in range(len(LANGS)):
        cells = []
        for _, c in rows:
            if j and c[j] == c[0]:
                cells.append("")
            elif j > 1 and c[j] == c[j - 1]:
                cells.append("^")
            else:
                cells.append(c[j])
        cols.append("~".join(cells))
    return "|".join(cols)


def js_rows(src, rows):
    """L('English text') → L(row): the page carries no English twice."""
    index = {cols[0]: n for n, (_, cols) in enumerate(rows)}

    def swap(m):
        text = m.group(1).replace("\\'", "'")
        if text not in index:
            raise SystemExit(f"web/iot.js: L({text!r}) is not in web/i18n.tsv")
        return f"L({index[text]}"
    return re.sub(r"\bL\('((?:[^'\\]|\\.)*)'", swap, src)


def minify_css(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"\s+", " ", src)
    src = re.sub(r"\s*([{};:,>!])\s*", r"\1", src)
    return src.replace(";}", "}").strip()


def minify_js(src):
    """Strips comments and spaces outside quotes.

    A space only stays between two letters/digits (else if, return x). It does
    not understand regex literals or template strings: the code in web/ uses none.
    """
    word = re.compile(r"[\w$]")
    out = []
    i, n = 0, len(src)
    gap = False
    while i < n:
        ch = src[i]
        if ch in "'\"":
            j = i + 1
            while src[j] != ch:
                if src[j] == "\n":
                    raise SystemExit("unclosed quote on the same line")
                j += 2 if src[j] == "\\" else 1
            tok, i = src[i:j + 1], j + 1
        elif src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
            gap = True
            continue
        elif ch == "/" or ch == "`":
            raise SystemExit(f"'{ch}' outside quotes: the minifier does not handle it")
        elif ch.isspace():
            gap = True
            i += 1
            continue
        else:
            tok, i = ch, i + 1
        if gap and out and word.match(out[-1][-1]) and word.match(tok[0]):
            out.append(" ")
        out.append(tok)
        gap = False
    js = "".join(out)
    # ;} → } outside quotes: splits at the quotes and replaces only in the code pieces
    parts = re.split(r"('(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\")", js)
    return "".join(p if k % 2 else p.replace(";}", "}") for k, p in enumerate(parts))


def build():
    rows = similar_first(load_table())
    out = {}
    for name, sym in FILES:
        with open(os.path.join(ROOT, "web", name), encoding="utf-8") as f:
            src = f.read()
        if name == "iot.js":
            src = js_rows(src, rows)
        if name == "i18n.js":
            src = src.replace("@MATRIX@", matrix(rows).replace("\\", "\\\\"))
        text = (minify_css if name.endswith(".css") else minify_js)(src).encode("utf-8")
        out[sym] = (name, text)
    return rows, out


def compress(data):
    if _zopfli:
        return _zopfli.compress(data, numiterations=1000)
    return gzip.compress(data, 9, mtime=0)


def array(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16])
            for i in range(0, len(data), 16)]
    body = ",\n    ".join(rows)
    return f"static const uint8_t {name}[] PROGMEM = {{\n    {body}\n}};\n"


def tag(text):
    return hashlib.sha256(text).hexdigest()[:8]


def header(assets):
    text = (
        LICENSE +
        "#pragma once\n"
        "// Generated by tools/embed_web.py from web/. Do not edit.\n"
        "#include <Arduino.h>\n"
    )
    for sym, (name, data) in assets.items():
        gz = compress(data)
        text += (
            f"\n// web/{name}: {len(data)} bytes minified, {len(gz)} gzipped\n"
            f'#define {sym}_TAG "{tag(data)}"\n'
            + array(sym + "_GZ", gz)
        )
    return text


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def i18n_header(rows):
    text = (
        LICENSE +
        "#pragma once\n"
        "// Generated by tools/embed_web.py from web/i18n.tsv. Do not edit.\n"
        "//\n"
        "// Each panel text is a marker that web/i18n.js swaps, in the browser, for the\n"
        "// text in the chosen language. String literals: they join others in a PSTR.\n\n"
    )
    width = max(len(k) for k, _ in rows)
    for n, (key, cols) in enumerate(rows):
        text += f'#define L_{key.ljust(width)} "{marker(n)}"   // {cols[0]}\n'
    return text


def test_header(rows):
    text = (
        LICENSE +
        "#pragma once\n"
        "// Generated by tools/embed_web.py from web/i18n.tsv. Do not edit.\n"
        "// English text of each marker, so the native tests read the pages in English.\n\n"
        "static const char* const I18N_EN[] = {\n"
    )
    text += "".join(f"    {c_string(cols[0])},\n" for _, cols in rows)
    return text + "};\n"


def check(rows, assets):
    errors = []
    for path, want in ((I18N_H, i18n_header(rows)), (TEST_EN, test_header(rows))):
        name = os.path.relpath(path, ROOT).replace(os.sep, "/")
        try:
            with open(path, encoding="utf-8") as f:
                if f.read().replace("\r\n", "\n") != want:
                    errors.append(f"{name} is out of date")
        except FileNotFoundError:
            errors.append(f"{name} does not exist")
    try:
        with open(OUT, encoding="utf-8") as f:
            have = f.read()
    except FileNotFoundError:
        return errors + ["WebAssets.h does not exist"]
    for sym, (name, data) in assets.items():
        m = re.search(r'#define %s_TAG "(\w+)"' % sym, have)
        a = re.search(r"%s_GZ\[\] PROGMEM = \{(.*?)\};" % sym, have, re.S)
        if not m or not a:
            errors.append(f"{sym} is not in WebAssets.h")
            continue
        gz = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", a.group(1)))
        if gzip.decompress(gz) != data:
            errors.append(f"web/{name} changed: WebAssets.h is out of date")
        elif m.group(1) != tag(data):
            errors.append(f"{sym}_TAG does not match web/{name}")
    return errors


def main():
    if "--min-iot" in sys.argv[1:]:
        with open(os.path.join(ROOT, "web", "iot.js"), encoding="utf-8") as f:
            text = minify_js(f.read())
        with open(sys.argv[sys.argv.index("--min-iot") + 1], "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return
    rows, assets = build()
    if "--check" in sys.argv[1:]:
        errors = check(rows, assets)
        for e in errors:
            print(e)
        if errors:
            print("Run: python tools/embed_web.py")
            sys.exit(1)
        print("WebAssets.h, I18n.h and test/i18n_en.h match web/")
        return
    for path, text in ((OUT, header(assets)), (I18N_H, i18n_header(rows)), (TEST_EN, test_header(rows))):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    for sym, (name, data) in assets.items():
        print(f"web/{name}: {len(data)} B, gzip {len(compress(data))} B")
    print(f"web/i18n.tsv: {len(rows)} texts in {len(LANGS)} languages"
          + ("" if _zopfli else " (zopfli not installed: zlib gzip)"))


if __name__ == "__main__":
    main()
