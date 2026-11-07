#!/usr/bin/env python3
"""A PlayStation data disc for the boot checks: ISO9660 on Mode 2 sectors, as a .bin and .cue.

usage: tests/boot/mkdisc.py <out.bin> <DIR/NAME.EXT=file> ...

Each argument is a path on the disc and the file to put there, one directory deep at most
(SCENE0/SCENE_0.SPK=...). A file named .STR is a movie in raw 2336-byte sectors, as psxavenc
writes it, and goes on the disc sector for sector with its own subheaders, so its audio stays
Form 2 and interleaved; anything else is Form 1 data. No boot files: the check loads the engine
with -loadexe and reads only these.
"""
import os
import struct
import sys

SECTOR, RAW, XA = 2352, 2048, 2336


def tables():
    edc = []
    for i in range(256):
        e = i
        for _ in range(8):
            e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
        edc.append(e)
    f, b = [0] * 256, [0] * 256
    for i in range(256):
        j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
        f[i] = j
        b[i ^ j] = i
    return edc, f, b


EDC, ECC_F, ECC_B = tables()


def edc(data):
    e = 0
    for byte in data:
        e = (e >> 8) ^ EDC[(e ^ byte) & 0xFF]
    return e


def ecc_block(s, major_count, minor_count, major_mult, minor_inc, dest):
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = s[12 + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        s[dest + major] = a
        s[dest + major + major_count] = a ^ b


def header(s, lba):
    s[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
    m, rest = divmod(lba + 150, 75 * 60)
    sec, fr = divmod(rest, 75)
    bcd = lambda v: (v // 10) << 4 | v % 10
    s[12:16] = bytes([bcd(m), bcd(sec), bcd(fr), 2])


def form1(lba, data, submode=0x08):
    s = bytearray(SECTOR)
    header(s, lba)
    s[16:24] = bytes([0, 0, submode, 0]) * 2
    s[24:24 + len(data)] = data
    struct.pack_into("<I", s, 2072, edc(s[16:2072]))
    saved = bytes(s[12:16])
    s[12:16] = b"\x00\x00\x00\x00"  # Mode 2's ECC is computed with the header as zeros
    ecc_block(s, 86, 24, 2, 86, 0x81C)
    ecc_block(s, 52, 43, 86, 88, 0x8C8)
    s[12:16] = saved
    return bytes(s)


def raw(lba, xa):
    s = bytearray(SECTOR)
    header(s, lba)
    s[16:16 + XA] = xa
    return bytes(s)


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


DATE = bytes([126, 11, 2, 0, 0, 0, 0])  # 2026-11-02


def record(name, lba, size, is_dir, attr):
    ident = name.encode("ascii") if isinstance(name, str) else name
    pad = b"" if len(ident) % 2 else b"\x00"
    xa = struct.pack(">HHH", 0, 0, attr) + b"XA" + bytes(6)
    body = (bytes([0]) + both32(lba) + both32(size) + DATE + bytes([2 if is_dir else 0, 0, 0]) + both16(1) +
            bytes([len(ident)]) + ident + pad + xa)
    return bytes([len(body) + 1]) + body


def main():
    out, items = sys.argv[1], sys.argv[2:]
    dirs = {}  # directory name -> [(file name, data, is movie)]
    for item in items:
        where, path = item.split("=", 1)
        d, name = where.rsplit("/", 1) if "/" in where else ("", where)
        with open(path, "rb") as fh:
            data = fh.read()
        movie = name.upper().endswith(".STR")
        if movie and len(data) % XA:
            sys.exit(f"{path}: not whole {XA}-byte sectors")
        dirs.setdefault(d, []).append((name.upper() + ";1", data, movie))
    subdirs = sorted(d for d in dirs if d)
    # Sectors: 0-15 system area, 16 the volume descriptor, 17 its terminator, 18 and 19 the two path
    # tables, then a sector per directory (root first), then the files.
    dir_lba = {"": 20}
    for i, d in enumerate(subdirs):
        dir_lba[d] = 21 + i
    lba = 21 + len(subdirs)
    files = []  # (lba, sectors, entry)
    for d in [""] + subdirs:
        for name, data, movie in sorted(dirs.get(d, [])):
            n = len(data) // XA if movie else max(1, -(-len(data) // RAW))
            files.append((d, name, lba, n, data, movie))
            lba += n
    total = lba

    def dir_sector(d):
        here, parent = dir_lba[d], dir_lba[""]
        recs = [record(b"\x00", here, RAW, True, 0x8D55), record(b"\x01", parent, RAW, True, 0x8D55)]
        if d == "":
            recs += [record(s, dir_lba[s], RAW, True, 0x8D55) for s in subdirs]
        for fd, name, flba, n, data, movie in files:
            if fd == d:
                recs.append(record(name, flba, n * RAW if movie else len(data), False, 0x3D55 if movie else 0x0D55))
        blob = b"".join(recs)
        if len(blob) > RAW:
            sys.exit(f"directory {d or '/'} needs more than one sector")
        return blob

    def path_table(big):
        p = b""
        for d, num in [("", 1)] + [(s, 1) for s in subdirs]:
            ident = d.encode("ascii") if d else b"\x00"
            p += bytes([len(ident), 0]) + struct.pack(">I" if big else "<I", dir_lba[d]) + \
                struct.pack(">H" if big else "<H", num) + ident + (b"\x00" if len(ident) % 2 else b"")
        return p

    lpt, mpt = path_table(False), path_table(True)
    pvd = bytearray(RAW)
    pvd[0:8] = b"\x01CD001\x01\x00"
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[40:72] = b"PSXSPLASH_BOOT".ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(RAW)
    pvd[132:140] = both32(len(lpt))
    struct.pack_into("<I", pvd, 140, 18)
    struct.pack_into(">I", pvd, 148, 19)
    pvd[156:190] = record(b"\x00", 20, RAW, True, 0x8D55)[:34]
    for o, n in ((190, 128), (318, 128), (446, 128), (574, 128), (702, 37), (739, 37), (776, 37)):
        pvd[o:o + n] = b" " * n
    pvd[813:830] = b"2026110200000000\x00"
    pvd[881] = 1
    pvd[1024:1032] = b"CD-XA001"
    vdst = b"\xffCD001\x01".ljust(RAW, b"\x00")

    with open(out, "wb") as fh:
        for i in range(16):
            fh.write(form1(i, bytes(RAW)))
        fh.write(form1(16, bytes(pvd), 0x09))
        fh.write(form1(17, vdst, 0x89))
        fh.write(form1(18, lpt.ljust(RAW, b"\x00"), 0x89))
        fh.write(form1(19, mpt.ljust(RAW, b"\x00"), 0x89))
        for d in [""] + subdirs:
            fh.write(form1(dir_lba[d], dir_sector(d).ljust(RAW, b"\x00"), 0x89))
        for fd, name, flba, n, data, movie in files:
            for k in range(n):
                if movie:
                    fh.write(raw(flba + k, data[k * XA:(k + 1) * XA]))
                else:
                    chunk = data[k * RAW:(k + 1) * RAW].ljust(RAW, b"\x00")
                    fh.write(form1(flba + k, chunk, 0x89 if k == n - 1 else 0x08))
    cue = os.path.splitext(out)[0] + ".cue"
    with open(cue, "w") as fh:
        fh.write(f'FILE "{os.path.basename(out)}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    print(f"{out}: {total} sectors, {len(files)} files")


if __name__ == "__main__":
    main()
