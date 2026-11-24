#!/usr/bin/env python3
"""Turn a splashpack v32 fixture into a v33 fixture with a spawn pool.

There is no editor support for spawn pools yet, so the boot test builds its pack
from the committed particles fixture. Run it with `python3 -I`:

    python3 -I tests/boot/tools/add_spawn_pool.py tests/boot/fixture-particles \\
        tests/boot/fixture-spawnpool \\
        --scene-lua tests/boot/scene/scripts/spawnpool_check.lua \\
        --copy-lua tests/boot/scene/scripts/spawnpool_copy.lua

What it does to scene_0.splashpack (the .spu and .vram files are copied as is):

  * version 32 -> 33, and the header grows from 172 to 176 bytes with
    spawnPoolTableOffset appended (src/splashpack.cpp, splashpackversion.hh);
  * the source object is cloned --clones times, appended to the object table and
    named <prefix>1, <prefix>2, ... (the fixture has one object, so there is no
    run to pick; with --clones 0 the longest run of objects whose names start
    with --prefix, or --first/--count, is used instead). The copies keep the
    source's ACTIVE flag on purpose: the loader must force them inactive;
  * the spawn pool table and its NUL-terminated name are written at the end of
    the pack, 4-byte aligned, the name an offset from the start of the pack like
    the sprite, audio and particle names;
  * the scene script is replaced by --scene-lua and --copy-lua is added as a new
    Lua file that the copies run (Lua is stored as source text in the pack).

Growing the header, the Lua file table and the object table moves everything
after them. Every absolute offset the engine reads is moved with it; this tool
knows where each one lives in the packs it accepts (the v32 particles fixture
shape: objects, a BVH, atlases, CLUTs, one sprite table, one particle table) and
refuses a pack that has anything else it would have to move, rather than
leaving a stale offset behind. The object name table is rewritten at the end of
the pack, since it gains names.
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

HDR_V32 = 172
HDR_V33 = 176
OBJ_SIZE = 92
LUA_ENTRY = 8
OFF_LUA_INDEX = 54   # GameObject::luaFileIndex
OFF_FLAGS = 56       # GameObject::flagsAsInt
OFF_POLY = 0         # GameObject::polygonsOffset

# Header fields (v32 layout), by byte offset.
H = dict(version=2, luaFileCount=4, gameObjectCount=6, atlasCount=8, clutCount=10, colliderCount=12,
         interactableCount=14, sceneLuaFileIndex=30, bvhNodeCount=32, bvhTriRefCount=34, triggerBoxCount=38,
         worldCollMeshCount=40, navRegionCount=44, nameTableOffset=60, audioClipCount=64, audioTableOffset=68,
         roomCount=78, cutsceneCount=84, cutsceneTableOffset=88, uiTableOffset=96, pixelDataOffset=100,
         animationCount=104, animationTableOffset=108, skinnedMeshCount=112, agentCount=114, skinTableOffset=116,
         memcardTableOffset=120, streamTableOffset=124, spriteTableOffset=128, spriteSheetCount=132,
         spriteAnimCount=134, tilemapTableOffset=140, lightTableOffset=144, shadowTableOffset=156,
         backgroundTableOffset=160, shineTableOffset=164, particleTableOffset=168)

# Counts or tables this tool cannot move offsets inside of: all must be zero.
MUST_BE_ZERO = ["colliderCount", "interactableCount", "triggerBoxCount", "worldCollMeshCount", "navRegionCount",
                "audioClipCount", "audioTableOffset", "roomCount", "cutsceneCount", "cutsceneTableOffset",
                "uiTableOffset", "pixelDataOffset", "animationCount", "animationTableOffset", "skinnedMeshCount",
                "agentCount", "skinTableOffset", "memcardTableOffset", "streamTableOffset", "tilemapTableOffset",
                "lightTableOffset", "shadowTableOffset", "backgroundTableOffset", "shineTableOffset"]


def fail(msg):
    print("add_spawn_pool: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", type=Path, help="fixture dir holding a v32 scene_0.splashpack")
    ap.add_argument("dst", type=Path, help="fixture dir to write (created)")
    ap.add_argument("--scene-lua", type=Path, required=True, help="Lua source that replaces the scene script")
    ap.add_argument("--copy-lua", type=Path, help="Lua source the pool copies run")
    ap.add_argument("--clone-from", type=int, default=0, help="object index to clone (default 0)")
    ap.add_argument("--clones", type=int, default=4, help="copies to append (default 4)")
    ap.add_argument("--prefix", default="Crate_", help="name prefix of the copies (default Crate_)")
    ap.add_argument("--name", default="crates", help="the pool's name (default crates)")
    ap.add_argument("--first", type=int, help="first object of the pool (default: found by --prefix)")
    ap.add_argument("--count", type=int, help="objects in the pool (default: found by --prefix)")
    a = ap.parse_args()

    d = (a.src / "scene_0.splashpack").read_bytes()
    u16 = lambda o: struct.unpack_from("<H", d, o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    if d[:2] != b"SP":
        fail("not a splashpack")
    if u16(H["version"]) != 32:
        fail("need a v32 pack, got v%d" % u16(H["version"]))
    for k in MUST_BE_ZERO:
        if (u16 if k.endswith("Count") else u32)(H[k]) != 0:
            fail("%s is set: this tool cannot relocate what it points at" % k)

    nlua, nobj = u16(H["luaFileCount"]), u16(H["gameObjectCount"])
    natlas, nclut = u16(H["atlasCount"]), u16(H["clutCount"])
    nbvh, nref = u16(H["bvhNodeCount"]), u16(H["bvhTriRefCount"])
    if not 0 <= a.clone_from < nobj:
        fail("--clone-from is not an object")
    if a.clones < 0:
        fail("--clones is negative")
    if a.copy_lua is None and a.clones:
        fail("--copy-lua is needed to give the copies a script")

    lua_at = HDR_V32
    lua_end = lua_at + nlua * LUA_ENTRY
    obj_end = lua_end + nobj * OBJ_SIZE
    atlas_at = obj_end + nbvh * 32 + nref * 4      # BVH nodes + triangle refs, then atlas, then CLUT records
    clut_at = atlas_at + natlas * 12

    # Everything that sat after an insertion moves by its size.
    ins_obj = a.clones * OBJ_SIZE
    ins_lua = LUA_ENTRY if a.copy_lua else 0

    def moved(old):
        """Where an old absolute offset points in the new pack."""
        if old == 0:
            return 0
        if old < lua_at:
            fail("offset %d points into the header" % old)
        return old + (HDR_V33 - HDR_V32) + (ins_lua if old >= lua_end else 0) + (ins_obj if old >= obj_end else 0)

    # ---- object names ----
    names = []
    p = u32(H["nameTableOffset"])
    if p == 0:
        fail("pack has no object names")
    for _ in range(nobj):
        n = d[p]
        names.append(d[p + 1:p + 1 + n].decode("ascii"))
        p += n + 2
    src_name = names[a.clone_from]
    new_names = names + [a.prefix + str(i + 1) for i in range(a.clones)]

    # ---- the pool: --first/--count, else the longest run sharing --prefix ----
    if a.first is not None or a.count is not None:
        if a.first is None or a.count is None:
            fail("--first and --count go together")
        first, count = a.first, a.count
    else:
        best = (0, 0)
        i = 0
        while i < len(new_names):
            j = i
            while j < len(new_names) and new_names[j].startswith(a.prefix):
                j += 1
            if j - i > best[1]:
                best = (i, j - i)
            i = max(j, i + 1)
        first, count = best
    if count == 0 or first + count > len(new_names):
        fail("no run of objects named %s* to use as a pool" % a.prefix)

    # ---- build the new pack ----
    out = bytearray(d)
    # 1. header: grow by one word, set at the end of the v32 header
    out[HDR_V32:HDR_V32] = b"\0\0\0\0"
    # 2. the Lua file table gains an entry (after the old entries, now at +4)
    if a.copy_lua:
        out[lua_end + 4:lua_end + 4] = b"\0" * LUA_ENTRY
    # 3. the object table gains the clones (after the old objects)
    src_obj_at = lua_end + (HDR_V33 - HDR_V32) + ins_lua + a.clone_from * OBJ_SIZE
    obj_end_new = obj_end + (HDR_V33 - HDR_V32) + ins_lua
    clone = bytes(out[src_obj_at:src_obj_at + OBJ_SIZE])
    out[obj_end_new:obj_end_new] = clone * a.clones

    def w16(o, v): struct.pack_into("<H", out, o, v)
    def w32(o, v): struct.pack_into("<I", out, o, v)
    def r32(o): return struct.unpack_from("<I", out, o)[0]

    # 4. move the offsets the engine reads
    for k in ("spriteTableOffset", "particleTableOffset"):
        w32(H[k], moved(u32(H[k])))
    lua_new = lua_at + (HDR_V33 - HDR_V32)
    for i in range(nlua):                                   # LuaFile::luaCodeOffset
        w32(lua_new + i * LUA_ENTRY, moved(u32(lua_at + i * LUA_ENTRY)))
    objs_new = lua_new + (nlua * LUA_ENTRY) + ins_lua
    for i in range(nobj + a.clones):                        # GameObject::polygonsOffset
        w32(objs_new + i * OBJ_SIZE + OFF_POLY, moved(r32(objs_new + i * OBJ_SIZE + OFF_POLY)))
    shift = (HDR_V33 - HDR_V32) + ins_lua + ins_obj
    for i in range(natlas):                                 # SPLASHPACKTextureAtlas::polygonsOffset
        o = atlas_at + shift + i * 12
        w32(o, moved(r32(o)))
    for i in range(nclut):                                  # SPLASHPACKClut::clutOffset
        o = clut_at + shift + i * 12
        w32(o, moved(r32(o)))
    sp = moved(u32(H["spriteTableOffset"]))
    if sp:                                                  # sheets (20 bytes) then anims (12), name offset first
        pos = sp
        for _ in range(u16(H["spriteSheetCount"])):
            w32(pos, moved(r32(pos))); pos += 20
        for _ in range(u16(H["spriteAnimCount"])):
            w32(pos, moved(r32(pos))); pos += 12
    pt = moved(u32(H["particleTableOffset"]))
    if pt:                                                  # u16 count, u16 reserved, 76-byte records, name offset first
        for i in range(struct.unpack_from("<H", out, pt)[0]):
            o = pt + 4 + i * 76
            w32(o, moved(r32(o)))

    # 5. counts and version
    w16(H["version"], 33)
    w16(H["luaFileCount"], nlua + (1 if a.copy_lua else 0))
    w16(H["gameObjectCount"], nobj + a.clones)

    # 6. clones: script of their own, active flag kept (the loader forces it off)
    for i in range(a.clones):
        o = objs_new + (nobj + i) * OBJ_SIZE
        if struct.unpack_from("<I", out, o + OFF_FLAGS)[0] & 1 == 0:
            fail("the source object is not exported active; the test needs that")
        struct.pack_into("<h", out, o + OFF_LUA_INDEX, nlua)

    # 7. new data at the end, each 4-byte aligned
    def append(blob):
        while len(out) % 4:
            out.append(0)
        at = len(out)
        out.extend(blob)
        return at

    nt = bytearray()
    for n in new_names:
        nt.append(len(n)); nt += n.encode("ascii") + b"\0"
    w32(H["nameTableOffset"], append(bytes(nt)))

    scene = a.scene_lua.read_bytes()
    w32(lua_new + u16(H["sceneLuaFileIndex"]) * LUA_ENTRY, append(scene))
    w32(lua_new + u16(H["sceneLuaFileIndex"]) * LUA_ENTRY + 4, len(scene))
    if a.copy_lua:
        copy = a.copy_lua.read_bytes()
        w32(lua_new + nlua * LUA_ENTRY, append(copy))
        w32(lua_new + nlua * LUA_ENTRY + 4, len(copy))

    pool_name = a.name.encode("ascii") + b"\0"
    table_at = append(struct.pack("<HHHHI", 1, 0, first, count, 0) )
    name_at = append(pool_name)
    w32(table_at + 8, name_at)
    w32(HDR_V32, table_at)                                  # spawnPoolTableOffset

    # ---- check what was written ----
    size = len(out)
    assert struct.unpack_from("<H", out, 2)[0] == 33
    for i in range(nlua + (1 if a.copy_lua else 0)):
        off, ln = struct.unpack_from("<II", out, lua_new + i * LUA_ENTRY)
        assert 0 < off and off + ln <= size, "lua %d" % i
    assert out[name_at:name_at + len(pool_name)] == pool_name
    assert table_at % 4 == 0 and first + count <= nobj + a.clones

    a.dst.mkdir(parents=True, exist_ok=True)
    for f in a.src.iterdir():
        if f.name != "scene_0.splashpack":
            shutil.copy(f, a.dst / f.name)
    (a.dst / "scene_0.splashpack").write_bytes(bytes(out))
    print("wrote %s: v33, %d bytes, %d objects (%d cloned from %r), pool %r = objects %d..%d, table at %d"
          % (a.dst / "scene_0.splashpack", size, nobj + a.clones, a.clones, src_name, a.name, first,
             first + count - 1, table_at))


if __name__ == "__main__":
    main()
