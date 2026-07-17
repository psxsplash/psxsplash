#!/usr/bin/env python3
"""Read-only splashpack inspector, plus a cross-language layout check.

Two jobs:

  dump    Print a pack's header and sprite tables. Answers "did the exporter
          actually write what I think it wrote?" without a console.

  check   Assert that the C++ reader (src/spritesystem.cpp, src/splashpack.cpp)
          and the C# writer (splashedit/Runtime/PSXSceneWriter.cs) agree on the
          v22 binary layout, field for field.

`check` exists because those two files are the only description of this format
and NOTHING compiles them together. A reordered field there does not fail to
build — it silently loads a sprite sheet with its CLUT where its cell size
should be, and the first symptom is garbled pixels on real hardware. This turns
that into a diff.

    python tools/splashpack_dump.py check
    python tools/splashpack_dump.py dump path/to/scene.splashpack
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENGINE = HERE.parent
REPO = ENGINE.parent

CPP_SPRITE = ENGINE / "src" / "spritesystem.cpp"
CPP_PACK = ENGINE / "src" / "splashpack.cpp"
CS_WRITER = REPO / "splashedit" / "Runtime" / "PSXSceneWriter.cs"

CTYPE_SIZE = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "char": 1}

# C# BinaryWriter call -> width in bytes. This is how the writer spells each
# field; `Write(byte)` and `Write((byte)0)` are both one byte.
CS_WIDTH = {"byte": 1, "ushort": 2, "uint": 4, "short": 2, "int": 4, "char": 1}


# ---------------------------------------------------------------------------
# Parsing the two sources of truth
# ---------------------------------------------------------------------------


def cpp_struct_fields(source: str, name: str) -> list[tuple[str, str]]:
    """[(field, ctype)] in declaration order, comments and padding included."""
    m = re.search(r"struct %s \{(.*?)\n\};" % re.escape(name), source, re.S)
    if not m:
        raise SystemExit(f"could not find 'struct {name}'")
    fields = []
    for line in m.group(1).splitlines():
        line = line.split("//")[0].strip().rstrip(";")
        if not line or line.startswith("static_assert"):
            continue
        ctype, names = line.split(None, 1)
        if ctype not in CTYPE_SIZE:
            raise SystemExit(f"{name}: unhandled C type {ctype!r}")
        for field in names.split(","):
            field = field.strip()
            # char magic[2] and friends
            arr = re.match(r"(\w+)\[(\d+)\]", field)
            if arr:
                fields.extend((f"{arr.group(1)}[{i}]", ctype) for i in range(int(arr.group(2))))
            else:
                fields.append((field, ctype))
    return fields


def cpp_layout(fields: list[tuple[str, str]]) -> tuple[list[tuple[int, str, str]], int]:
    """Offsets, assuming natural alignment — which every struct here satisfies."""
    out, off = [], 0
    for field, ctype in fields:
        size = CTYPE_SIZE[ctype]
        if off % size:
            raise SystemExit(
                f"field {field!r} at +{off} is not {size}-byte aligned; "
                "the compiler would insert padding the writer does not"
            )
        out.append((off, ctype, field))
        off += size
    return out, off


def cs_writes(source: str, start_marker: str, end_marker: str) -> list[tuple[int, str]]:
    """[(width, text)] for each writer.Write(...) between two markers."""
    a = source.index(start_marker)
    b = source.index(end_marker, a)
    block = source[a:b]

    out = []
    for line in block.splitlines():
        line = line.split("//")[0].strip()
        m = re.search(r"writer\.Write\((.*)\)\s*;", line)
        if not m:
            continue
        arg = m.group(1).strip()
        cast = re.match(r"\((\w+)\)", arg)
        if cast and cast.group(1) in CS_WIDTH:
            out.append((CS_WIDTH[cast.group(1)], arg))
        else:
            out.append((None, arg))  # width from the field's declared C# type
    return out


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

# How each C# record field is declared, so an uncast Write(s.Foo) has a width.
SHEET_CS_TYPES = {
    "TexpageX": 1, "TexpageY": 1, "U0": 1, "V0": 1,
    "ClutX": 2, "ClutY": 2,
    "CellW": 1, "CellH": 1, "Cols": 1, "Rows": 1,
    "BitDepthIndex": 1,
}
ANIM_CS_TYPES = {
    "Sheet": 1, "FirstFrame": 1, "FrameCount": 1, "FrameDuration": 1,
}


def resolve_widths(writes, declared):
    out = []
    for width, arg in writes:
        if width is None:
            field = arg.split(".")[-1]
            if field not in declared:
                raise SystemExit(f"unknown C# field width for {arg!r}")
            width = declared[field]
        out.append((width, arg))
    return out


def compare(name, cpp_fields_, cs_writes_, declared):
    layout, total = cpp_layout(cpp_fields_)
    resolved = resolve_widths(cs_writes_, declared)

    print(f"{name}: {total} bytes")
    problems = []
    if len(layout) != len(resolved):
        problems.append(
            f"  field count differs: C++ has {len(layout)}, C# writes {len(resolved)}"
        )

    off = 0
    for i, (cpp_off, ctype, field) in enumerate(layout):
        if i >= len(resolved):
            problems.append(f"  +{cpp_off:3d} {field}: C# never writes it")
            continue
        width, arg = resolved[i]
        size = CTYPE_SIZE[ctype]
        mark = "ok " if (width == size and off == cpp_off) else "BAD"
        if mark == "BAD":
            problems.append(
                f"  +{cpp_off:3d} {ctype} {field}: C++ wants {size}B at +{cpp_off}, "
                f"C# writes {width}B at +{off} ({arg})"
            )
        print(f"  {mark} +{cpp_off:3d} {ctype:9s} {field:16s} <- {arg}")
        off += width

    if off != total:
        problems.append(f"  total size differs: C++ {total}B, C# writes {off}B")

    return problems


def cmd_check() -> int:
    cpp_sprite = CPP_SPRITE.read_text(encoding="utf-8")
    cpp_pack = CPP_PACK.read_text(encoding="utf-8")
    cs = CS_WRITER.read_text(encoding="utf-8")

    problems = []

    problems += compare(
        "SPLASHPACKSpriteSheet",
        cpp_struct_fields(cpp_sprite, "SPLASHPACKSpriteSheet"),
        cs_writes(cs, "foreach (var s in spriteSheetData)", "foreach (var a in spriteAnimData)"),
        SHEET_CS_TYPES,
    )
    print()
    problems += compare(
        "SPLASHPACKSpriteAnim",
        cpp_struct_fields(cpp_sprite, "SPLASHPACKSpriteAnim"),
        cs_writes(cs, "foreach (var a in spriteAnimData)", "// Name strings, then backfill"),
        ANIM_CS_TYPES,
    )
    print()

    # The header mixes in psyqo types, so rather than model those, lean on the
    # contract the engine already states about itself — that static_assert is
    # what actually fails the build if the header drifts.
    m = re.search(r'static_assert\(sizeof\(SPLASHPACKFileHeader\) == (\d+)', cpp_pack)
    header_size = int(m.group(1)) if m else -1
    print(f"SPLASHPACKFileHeader: {header_size} bytes (per its static_assert)")
    if header_size != 144:
        problems.append(f"  header is {header_size}B; v22 must be 144")

    # The v22 tail must be exactly what cmd_dump unpacks at +128.
    for field in ("spriteTableOffset", "spriteSheetCount", "spriteAnimCount", "sceneHash"):
        if field not in cpp_pack:
            problems.append(f"  header is missing the v22 field {field!r}")

    m = re.search(r"writer\.Write\(\(ushort\)(\d+)\);", cs)
    cs_version = int(m.group(1)) if m else -1
    print(f"  C# writes version {cs_version}")
    if cs_version != 22:
        problems.append(f"  C# writes version {cs_version}, expected 22")

    # The runtime must still accept older packs.
    if 'header->version >= 22' not in cpp_pack:
        problems.append("  C++ does not gate the v22 fields on version >= 22")
    if 'kSplashpackHeaderSizeV21' not in cpp_pack:
        problems.append("  C++ lost the v21 header size; older packs would misparse")

    print()
    if problems:
        print("LAYOUT MISMATCH:")
        for p in problems:
            print(p)
        return 1
    print("C++ reader and C# writer agree on the v22 sprite layout.")
    return 0


# ---------------------------------------------------------------------------
# dump
# ---------------------------------------------------------------------------


def cmd_dump(path: Path) -> int:
    data = path.read_bytes()
    if data[:2] != b"SP":
        print(f"not a splashpack: magic is {data[:2]!r}, expected b'SP'")
        return 1

    version = struct.unpack_from("<H", data, 2)[0]
    print(f"{path.name}: {len(data)} bytes, splashpack v{version}")
    if version < 22:
        print("  (pre-v22: no sprite tables, no authored scene hash)")
        return 0

    sprite_off, sheets, anims, scene_hash = struct.unpack_from("<IHHI", data, 128)
    print(f"  sceneHash        0x{scene_hash:08X}" + ("  (0 = derive at runtime)" if not scene_hash else ""))
    print(f"  spriteSheetCount {sheets}")
    print(f"  spriteAnimCount  {anims}")
    print(f"  spriteTableOffset 0x{sprite_off:X}")

    if not sprite_off or not sheets:
        return 0

    def name_at(off):
        if not off or off >= len(data):
            return "<none>"
        end = data.index(b"\0", off)
        return data[off:end].decode("utf-8", "replace")

    print("\n  sheets:")
    off = sprite_off
    for i in range(sheets):
        (name_off, tpx, tpy, u0, v0, clx, cly, cw, ch, cols, rows, bpp,
         _p0, _p1) = struct.unpack_from("<IBBBBHHBBBBBBH", data, off)
        depth = {0: "4bpp", 1: "8bpp", 2: "16bpp"}.get(bpp, f"?{bpp}")
        print(f"    [{i}] {name_at(name_off)!r} {depth} grid={cols}x{rows} "
              f"cell={cw}x{ch} tpage=({tpx},{tpy}) uv0=({u0},{v0}) clut=({clx},{cly})")
        off += 20

    print("\n  animations:")
    for i in range(anims):
        name_off, sheet, first, count, dur, loop, _p0, _p1 = struct.unpack_from(
            "<IBBBBBBH", data, off)
        secs = (count * dur) / 60.0 if count else 0
        print(f"    [{i}] {name_at(name_off)!r} sheet={sheet} frames={first}..{first + count - 1} "
              f"hold={dur}f {'loop' if loop else 'once'} ({secs:.2f}s/cycle)")
        off += 12

    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "check":
        return cmd_check()
    if sys.argv[1] == "dump":
        if len(sys.argv) < 3:
            print("dump needs a path to a .splashpack")
            return 2
        return cmd_dump(Path(sys.argv[2]))
    print(f"unknown command {sys.argv[1]!r}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
