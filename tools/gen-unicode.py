#!/usr/bin/env python3
"""Write unicode_tables.h: the tables terminal.c uses to compose a cell's
marks and to leave invisible characters undrawn.

    python3 tools/gen-unicode.py [DerivedCoreProperties.txt] > unicode_tables.h

- compose_pairs: every primary composite of Unicode canonical composition,
  as (first, second, composite), sorted. A pair is kept when NFC of its
  canonical decomposition gives the composite back, which leaves out the
  composition exclusions and the singletons. Hangul syllables are composed
  arithmetically in terminal.c, not listed.
- combining_classes: ranges of code points with a canonical combining class
  other than 0, as (first, last, class).
- ignorable_ranges: Default_Ignorable_Code_Point from the Unicode
  Character Database, less U+00AD SOFT HYPHEN, which a terminal shows as a
  hyphen in a cell of its own, plus U+FFF9 to U+FFFB, the interlinear
  annotation controls, which are format characters with no glyph but not
  default ignorable.

Python's unicodedata and the DerivedCoreProperties.txt read must be the
same Unicode version; the version is written into the header.
"""
import re
import sys
import unicodedata

UCD = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/unicode/DerivedCoreProperties.txt"
SHOWN = {0x00AD}
HIDDEN = set(range(0xFFF9, 0xFFFC))


def ranges(cps, value=lambda c: None):
    out = []
    for c in sorted(cps):
        v = value(c)
        if out and out[-1][1] == c - 1 and out[-1][2] == v:
            out[-1][1] = c
        else:
            out.append([c, c, v])
    return out


def main():
    text = open(UCD, encoding="utf-8").read()
    m = re.search(r"DerivedCoreProperties-(\d+\.\d+\.\d+)\.txt", text)
    if not m or m.group(1) != unicodedata.unidata_version:
        raise SystemExit(f"{UCD} is Unicode {m and m.group(1)}, unicodedata is {unicodedata.unidata_version}")

    pairs = []
    for cp in range(0x110000):
        if 0xAC00 <= cp <= 0xD7A3:
            continue
        d = unicodedata.decomposition(chr(cp))
        if not d or d.startswith("<"):
            continue
        parts = [int(p, 16) for p in d.split()]
        if len(parts) == 2 and unicodedata.normalize("NFC", chr(parts[0]) + chr(parts[1])) == chr(cp):
            pairs.append((parts[0], parts[1], cp))
    pairs.sort()

    ccc = ranges((c for c in range(0x110000) if unicodedata.combining(chr(c))),
                 lambda c: unicodedata.combining(chr(c)))

    ign = set()
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        if not line.endswith("Default_Ignorable_Code_Point"):
            continue
        r = line.split(";")[0].strip()
        a, _, b = r.partition("..")
        ign.update(range(int(a, 16), int(b or a, 16) + 1))
    ign = (ign - SHOWN) | HIDDEN
    ign = ranges(ign)

    w = sys.stdout.write
    w(f"/* Made by tools/gen-unicode.py from Unicode {unicodedata.unidata_version}. Do not edit. */\n\n")
    w("static const struct { uint32_t first, second, composite; } compose_pairs[] = {\n")
    for a, b, c in pairs:
        w(f"    {{0x{a:04X}, 0x{b:04X}, 0x{c:04X}}},\n")
    w("};\n\n")
    w("static const struct { uint32_t first, last; uint8_t ccc; } combining_classes[] = {\n")
    for a, b, v in ccc:
        w(f"    {{0x{a:04X}, 0x{b:04X}, {v}}},\n")
    w("};\n\n")
    w("static const struct { uint32_t first, last; } ignorable_ranges[] = {\n")
    for a, b, _ in ign:
        w(f"    {{0x{a:04X}, 0x{b:04X}}},\n")
    w("};\n")


if __name__ == "__main__":
    main()
