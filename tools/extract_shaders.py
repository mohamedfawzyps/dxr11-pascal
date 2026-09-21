#!/usr/bin/env python3
"""Pull the Phase 2 HLSL out of raytest.cpp into standalone .hlsl files.

Phase 2 proved these four shaders equivalent in pairs, bit-exactly, on real
hardware. That makes them the specification for what the Phase 5 rewriter has
to consume and produce, so Phase 5 must read the SAME text rather than a copy
that can quietly drift. Hence extraction rather than duplication.

Run from the repository root:
    python tools/extract_shaders.py
"""

import io
import os
import re
import sys

SRC = os.path.join("phase2", "raytest.cpp")
DST = os.path.join("phase5", "shaders")

# C++ identifier in raytest.cpp -> the file we write, and what it is for.
WANTED = {
    "kRayQueryHLSL":      ("rayquery_opaque.hlsl",  "INPUT  pattern 1, opaque closest hit"),
    "kTraceRayHLSL":      ("traceray_opaque.hlsl",  "TARGET pattern 1, opaque closest hit"),
    "kRayQueryAlphaHLSL": ("rayquery_alpha.hlsl",   "INPUT  pattern 3, alpha-tested closest hit"),
    "kTraceRayAlphaHLSL": ("traceray_alpha.hlsl",   "TARGET pattern 3, alpha-tested closest hit"),
}

LITERAL = re.compile(
    r'static\s+const\s+char\*\s+(\w+)\s*=\s*R"HLSL\((.*?)\)HLSL";',
    re.DOTALL)


def main():
    if not os.path.isfile(SRC):
        sys.exit("run me from the repository root: %s not found" % SRC)

    text = io.open(SRC, encoding="utf-8").read()
    found = {m.group(1): m.group(2) for m in LITERAL.finditer(text)}

    missing = [k for k in WANTED if k not in found]
    if missing:
        sys.exit("raytest.cpp no longer defines: %s" % ", ".join(missing))

    os.makedirs(DST, exist_ok=True)
    for name, (filename, what) in sorted(WANTED.items()):
        body = found[name].lstrip("\n")
        header = ("// %s\n"
                  "// EXTRACTED from phase2/raytest.cpp (%s) by tools/extract_shaders.py.\n"
                  "// Do not edit here. Edit raytest.cpp and re-extract, so the shader\n"
                  "// Phase 5 works on stays the one Phase 2 actually validated.\n\n"
                  % (what, name))
        path = os.path.join(DST, filename)
        io.open(path, "w", encoding="utf-8", newline="\n").write(header + body)
        print("%-24s <- %s" % (filename, name))


if __name__ == "__main__":
    main()
