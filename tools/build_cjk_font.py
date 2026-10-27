#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build assets/fonts/MaterializrSansSC-UI.ttf - the UI's CJK fallback.

The UI font is JetBrains Mono, which has no CJK glyphs. This script derives a
small TrueType fallback from Noto Sans SC so Chinese catalogue strings have
real glyphs to render (see src/app/Application.cpp, the MergeMode font load).
It is a build-time tool, not part of the app build: the generated TTF is
committed.

Noto Sans SC upstream is a variable CFF-free TrueType (glyf). We pin wght=400
and subset to GB2312 plus the punctuation/symbol ranges listed below, which is
~2.1 MB instead of ~10 MB for the full family. All Simplified Chinese strings
in tools/i18n_catalogue.py's ZH table are checked to be inside that set.

The output is a MODIFIED version of Noto Sans SC. It is renamed to
"Materializr Sans SC UI" because the upstream OFL reserves the name "Source";
the copyright and license name records are preserved. Ship
assets/fonts/licenses/NotoSansSC-OFL.txt with it.

Usage:
    python3 tools/build_cjk_font.py /path/to/NotoSansSC[wght].ttf [out.ttf]

Requires fonttools (pip install fonttools). The upstream variable font is at
https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_OUT = os.path.join(ROOT, "assets", "fonts", "MaterializrSansSC-UI.ttf")


def gb2312_unicodes():
    """Every character GB2312 can encode (6,763 hanzi + symbols)."""
    out = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([hi, lo]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return out


# Latin, typography and symbols the UI or its translations use. The CJK
# punctuation and fullwidth ranges are what Chinese text needs; the rest makes
# the fallback self-sufficient if a glyph is ever missing from JetBrains Mono.
EXTRA_RANGES = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x00FF),  # Latin-1
    (0x0370, 0x03FF),  # Greek (the UI uses Delta)
    (0x2000, 0x206F),  # general punctuation (dashes, quotes, ellipsis)
    (0x2190, 0x21FF),  # arrows
    (0x2200, 0x22FF),  # maths (minus, intersection, union)
    (0x25A0, 0x25FF),  # geometric shapes (bullet variants)
    (0x2600, 0x26FF),  # miscellaneous symbols (warning)
    (0x3000, 0x303F),  # CJK punctuation
    (0xFE30, 0xFE4F),  # CJK compatibility forms
    (0xFF00, 0xFFEF),  # fullwidth forms
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT

    font = TTFont(src)
    if "fvar" in font:
        instantiateVariableFont(font, {"wght": 400}, inplace=True,
                                updateFontNames=False)

    # OFL: 'Source' is a Reserved Font Name, so the derived font is renamed.
    names = {1: "Materializr Sans SC UI", 2: "Regular",
             4: "Materializr Sans SC UI", 6: "MaterializrSansSC-UI",
             16: "Materializr Sans SC UI", 17: "Regular"}
    for rec in font["name"].names:
        if rec.nameID in names:
            text = names[rec.nameID]
            rec.string = (text.encode("utf-16-be") if rec.platformID == 3
                          else text.encode("latin-1", "replace"))

    unicodes = gb2312_unicodes()
    for lo, hi in EXTRA_RANGES:
        unicodes.update(range(lo, hi + 1))

    # Every Chinese string must survive the subset, or the app shows boxes.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from i18n_catalogue import ZH
    needed = {ord(ch) for value in ZH.values() for ch in value
              if ch not in "\n\r\t"}
    missing = sorted(needed - unicodes)
    if missing:
        sys.exit("CJK font would miss %d translated character(s): %r"
                 % (len(missing), "".join(chr(c) for c in missing[:40])))

    opts = subset.Options()
    opts.layout_features = []       # ImGui does no complex shaping
    opts.name_IDs = ["*"]           # keep copyright + license notices
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.drop_tables += ["BASE", "GDEF", "GPOS", "GSUB", "vhea", "vmtx",
                         "STAT", "HVAR", "MVAR", "avar", "fvar", "gvar"]

    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)

    # A requested codepoint is not a delivered glyph: Noto Sans SC has no
    # U+25B8, for example. That is fine only when the PRIMARY font covers it,
    # so check the actual subset cmap against JetBrains Mono's cmap.
    primary_cmap = set()
    primary = os.path.join(ROOT, "assets", "fonts", "JetBrainsMono-Regular.ttf")
    if os.path.exists(primary):
        primary_cmap = set(TTFont(primary).getBestCmap())
    cmap = set(font.getBestCmap())
    uncovered = sorted(cp for cp in needed
                       if cp not in cmap and cp not in primary_cmap)
    if uncovered:
        sys.exit("CJK font and JetBrains Mono both lack U+%04X (%r)"
                 % (uncovered[0], chr(uncovered[0])))

    os.makedirs(os.path.dirname(out), exist_ok=True)
    subset.save_font(font, out, opts)
    print("wrote %s (%.2f MB, %d codepoints)"
          % (out, os.path.getsize(out) / 1024.0 / 1024.0, len(unicodes)))


if __name__ == "__main__":
    main()
