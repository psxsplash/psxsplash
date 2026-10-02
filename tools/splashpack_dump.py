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
CPP_PACK_HH = ENGINE / "src" / "splashpack.hh"
CPP_TILE = ENGINE / "src" / "tilesystem.cpp"
CPP_TILEMATH = ENGINE / "src" / "tilemath.hh"
CPP_UI = ENGINE / "src" / "uisystem.cpp"
CS_WRITER = REPO / "splashedit" / "Runtime" / "PSXSceneWriter.cs"

CTYPE_SIZE = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "int32_t": 4, "char": 1}

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
# v23 tilemap records.
TILEMAP_CS_TYPES = {
    "Width": 2, "Height": 2, "TileW": 1, "TileH": 1, "TilesetSheet": 1,
}
TILEOBJ_CS_TYPES = {
    "Kind": 1, "Id": 1, "TileX": 2, "TileY": 2,
}
# v24 point lights: every write is cast, so nothing to declare.
LIGHT_CS_TYPES: dict[str, int] = {}


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


def ui_image_cpp(source: str) -> list[tuple[int, int, str]]:
    """[(offset, width, field)] the C++ reads out of an Image element's 16-byte
    type-specific block, in source order.

    Parsed from `uisystem.cpp`'s `case UIElementType::Image:` block, which reads
    either `typeData[n]` (one byte) or `*reinterpret_cast<uint16_t*>(&typeData[n])`
    (two).
    """
    m = re.search(r"case UIElementType::Image:(.*?)\bbreak;", source, re.S)
    if not m:
        raise SystemExit("uisystem.cpp has no `case UIElementType::Image:` block")

    out = []
    for line in m.group(1).splitlines():
        line = line.split("//")[0]
        hit = re.search(
            r"el\.image\.(\w+)\s*=\s*(?:\*reinterpret_cast<(uint16_t)\*>\(&)?typeData\[(\d+)\]",
            line)
        if hit:
            field, cast, off = hit.groups()
            out.append((int(off), 2 if cast else 1, field))
    return out


def ui_image_cs(source: str) -> list[tuple[int, int, str]]:
    """The same list from the C# writer, read off its `// [n]` / `// [n-m]`
    offset comments in `case PSXUIElementType.Image:`."""
    m = re.search(r"case PSXUIElementType\.Image:(.*?)\bbreak;", source, re.S)
    if not m:
        raise SystemExit("PSXSceneWriter.cs has no `case PSXUIElementType.Image:` block")

    out = []
    for line in m.group(1).splitlines():
        hit = re.search(r"writer\.Write\(el\.(\w+)\);\s*//\s*\[(\d+)(?:-(\d+))?\]", line)
        if hit:
            field, lo, hi = hit.groups()
            lo = int(lo)
            out.append((lo, (int(hi) - lo + 1) if hi else 1, field))
    return out


def check_ui_image(cpp_ui: str, cs: str) -> list[str]:
    """The Image element payload is the ONE record whose two ends are hand-written
    byte offsets rather than a struct, so nothing but this compares them.

    It also carries the sheet grid (cellW/cellH/cols/baseU/baseV) that makes
    `UI.SetFrame` work, packed into what used to be padding. Getting one of those
    five off by a byte does not fail to build: it points a digit at the wrong cell
    of the atlas, on hardware only.
    """
    cpp = ui_image_cpp(cpp_ui)
    cs_fields = ui_image_cs(cs)

    print(f"UIElement Image payload: 16 bytes "
          f"(C++ reads {len(cpp)} fields, C# writes {len(cs_fields)})")

    problems = []
    if len(cpp) != len(cs_fields):
        problems.append(f"  field count differs: C++ reads {len(cpp)}, C# writes {len(cs_fields)}")

    for i, (off, width, field) in enumerate(cpp):
        if i >= len(cs_fields):
            problems.append(f"  +{off:2d} {field}: C# never writes it")
            continue
        c_off, c_width, c_field = cs_fields[i]
        # Names too, not just offsets: every grid byte is one byte at a
        # consecutive offset, so swapping two of them is invisible to a pure
        # offset diff and produces a panel drawn from the wrong cells.
        alias = {"cols": "sheetcols", "bitdepth": "bitdepthindex"}
        want = alias.get(field.lower(), field.lower())
        ok = (off == c_off and width == c_width and want == c_field.lower())
        if not ok:
            problems.append(f"  +{off:2d} {field}: C++ reads {width}B at +{off}, "
                            f"C# writes {c_width}B at +{c_off} ({c_field})")
        print(f"  {'ok ' if ok else 'BAD'} +{off:2d} {width}B {field:10s} <- {c_field}")

    # The five grid bytes are the whole point of the sheet-backed image, and they
    # live in bytes 11..15. If they ever drift back to padding, SetFrame silently
    # becomes a no-op on every element.
    grid = {f for _, _, f in cpp} & {"cellW", "cellH", "cols", "baseU", "baseV"}
    if len(grid) != 5:
        problems.append(f"  C++ lost part of the sheet grid: has {sorted(grid)}")
    covered = sum(w for _, w, _ in cpp)
    if covered > 16:
        problems.append(f"  the payload reads {covered}B but the record only has 16")

    return problems


def cmd_check() -> int:
    cpp_sprite = CPP_SPRITE.read_text(encoding="utf-8")
    cpp_pack = CPP_PACK.read_text(encoding="utf-8")
    cpp_tile = CPP_TILE.read_text(encoding="utf-8")
    cpp_tilemath = CPP_TILEMATH.read_text(encoding="utf-8")
    cpp_ui = CPP_UI.read_text(encoding="utf-8")
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
    problems += compare(
        "SPLASHPACKTilemap",
        cpp_struct_fields(cpp_tile, "SPLASHPACKTilemap"),
        cs_writes(cs, "// SPLASHPACKTilemap header (20 bytes).", "// Cells: 2 bytes each"),
        TILEMAP_CS_TYPES,
    )
    print()
    problems += compare(
        "TileObject",
        cpp_struct_fields(cpp_tilemath, "TileObject"),
        cs_writes(cs, "foreach (var o in tm.Objects)", "writer.Seek((int)cellsOffsetPos"),
        TILEOBJ_CS_TYPES,
    )
    print()
    problems += check_ui_image(cpp_ui, cs)
    print()
    problems += compare(
        "SPLASHPACKPointLight",
        cpp_struct_fields(CPP_PACK_HH.read_text(encoding="utf-8"), "SPLASHPACKPointLight"),
        cs_writes(cs, "// SPLASHPACKPointLight record", "// end SPLASHPACKPointLight"),
        LIGHT_CS_TYPES,
    )
    print()

    # The header mixes in psyqo types, so rather than model those, lean on the
    # contract the engine already states about itself — that static_assert is
    # what actually fails the build if the header drifts.
    m = re.search(r'static_assert\(sizeof\(SPLASHPACKFileHeader\) == (\d+)', cpp_pack)
    header_size = int(m.group(1)) if m else -1
    print(f"SPLASHPACKFileHeader: {header_size} bytes (per its static_assert)")
    if header_size != 148:
        problems.append(f"  header is {header_size}B; v24 appends lightTableOffset to the 144B v23 header")

    # The v22 tail must be exactly what cmd_dump unpacks at +128; v23 reuses the
    # final reserved word as tilemapTableOffset, so the header does not grow.
    for field in ("spriteTableOffset", "spriteSheetCount", "spriteAnimCount", "sceneHash",
                  "tilemapTableOffset", "lightTableOffset"):
        if field not in cpp_pack:
            problems.append(f"  header is missing the field {field!r}")

    # A scene with no point lights is written as v23 (byte-identical to before
    # lights existed); one with lights as v24.
    m = re.search(r"writer\.Write\(\(ushort\)\(hasLights \? (\d+) : (\d+)\)\);", cs)
    cs_versions = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
    print(f"  C# writes version {cs_versions[0]} with point lights, {cs_versions[1]} without")
    if cs_versions != (24, 23):
        problems.append(f"  C# writes versions {cs_versions}, expected (24, 23)")
    if "if (hasLights)\n                    writer.Write((uint)0);                      // lightTableOffset placeholder" not in cs:
        problems.append("  C# does not write lightTableOffset only for v24")

    # The runtime must still accept older packs.
    if 'header->version >= 22' not in cpp_pack:
        problems.append("  C++ does not gate the v22 fields on version >= 22")
    if 'header->version >= 23' not in cpp_pack:
        problems.append("  C++ does not gate the tilemap offset on version >= 23")
    if 'header->version >= 24' not in cpp_pack:
        problems.append("  C++ does not gate the light table on version >= 24")
    if 'kSplashpackHeaderSizeV22' not in cpp_pack:
        problems.append("  C++ lost the v22 header size; v22/v23 packs would misparse")
    if 'kSplashpackHeaderSizeV21' not in cpp_pack:
        problems.append("  C++ lost the v21 header size; older packs would misparse")

    print()
    if problems:
        print("LAYOUT MISMATCH:")
        for p in problems:
            print(p)
        return 1
    print("C++ reader and C# writer agree on the v24 sprite, tilemap and light layout.")
    return 0


# ---------------------------------------------------------------------------
# dump
# ---------------------------------------------------------------------------


def dump_tilemap(data: bytes, off: int) -> None:
    (width, height, tile_w, tile_h, sheet, _p0, obj_count, _p1,
     cells_off, objects_off) = struct.unpack_from("<HHBBBBHHII", data, off)
    print("\n  tilemap:")
    print(f"    {width}x{height} tiles, cell={tile_w}x{tile_h}, tilesetSheet={sheet}")
    print(f"    cells   0x{cells_off:X} ({width * height} cells, {width * height * 2} bytes)")
    print(f"    objects 0x{objects_off:X} ({obj_count})")

    # Walkability histogram: a quick "did I actually paint walls?" sanity check
    # that needs no console.
    if cells_off and cells_off + width * height * 2 <= len(data):
        empty = solid = walk = 0
        for i in range(width * height):
            tile, flags = data[cells_off + i * 2], data[cells_off + i * 2 + 1]
            if tile == 0xFF:
                empty += 1
            elif flags & 0x01:
                walk += 1
            else:
                solid += 1
        print(f"    cells: {walk} walkable, {solid} solid, {empty} empty")

    if objects_off and obj_count:
        print("    object list:")
        # kind is an opaque game-defined byte; the engine (and this tool) never
        # interpret it, so print the number.
        for i in range(obj_count):
            kind, oid, tx, ty = struct.unpack_from("<BBHH", data, objects_off + i * 6)
            print(f"      [{i}] kind={kind} id={oid} at tile ({tx},{ty})")


def dump_lights(data: bytes, off: int) -> None:
    (count,) = struct.unpack_from("<H", data, off)
    print(f"\n  point lights: {count}")
    for i in range(count):
        x, y, z, radius, inten, r, g, b, flags, _pad, name_off = struct.unpack_from(
            "<iiiiHBBBBHI", data, off + 4 + 28 * i)
        name = "<none>"
        if name_off and name_off < len(data):
            name = data[name_off:data.index(b"\0", name_off)].decode("utf-8", "replace")
        print(f"    [{i}] {name!r} pos=({x / 4096:.3f},{y / 4096:.3f},{z / 4096:.3f}) "
              f"radius={radius / 4096:.3f} intensity={inten / 4096:.3f} rgb=({r},{g},{b}) "
              f"{'on' if flags & 1 else 'off'}")


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

    sprite_off, sheets, anims, scene_hash, tilemap_off = struct.unpack_from("<IHHII", data, 128)
    print(f"  sceneHash        0x{scene_hash:08X}" + ("  (0 = derive at runtime)" if not scene_hash else ""))
    print(f"  spriteSheetCount {sheets}")
    print(f"  spriteAnimCount  {anims}")
    print(f"  spriteTableOffset 0x{sprite_off:X}")
    print(f"  tilemapTableOffset 0x{tilemap_off:X}" + ("  (0 = no tilemap)" if not tilemap_off else ""))
    if version >= 23 and tilemap_off:
        dump_tilemap(data, tilemap_off)
    if version >= 24:
        (light_off,) = struct.unpack_from("<I", data, 144)
        print(f"  lightTableOffset 0x{light_off:X}" + ("  (0 = no lights)" if not light_off else ""))
        if light_off:
            dump_lights(data, light_off)

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
