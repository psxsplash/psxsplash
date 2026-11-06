#!/usr/bin/env python3
"""Build a psxsplash disc image for LOADER=archive with PCSX-Redux's `authoring` tool.

usage: tools/mkdisc.py <authoring> <psxsplash.ps-exe> <export dir> <out.bin> [volume id]

The export dir holds the files SplashEdit writes for PCdrv (scene_0.splashpack,
scene_0.vram, ...). Each one goes into the archive under its own name, which is
the name the engine asks for, in the order loadScene reads a scene's files.
The image boots on a console and in an emulator, and the same image placed in
the PCdrv directory as psxsplash.bin is what the engine reads when there is no
disc.
"""

import json
import os
import subprocess
import sys
import tempfile

# SceneManager::loadScene reads the loading screen, VRAM, SPU, splashpack, then
# the streamed geometry; keeping that order keeps the drive reading forwards.
KINDS = ["loading", "vram", "spu", "splashpack", "geo"]


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__.strip().splitlines()[2])
    authoring, exe, export, out = (os.path.abspath(a) for a in sys.argv[1:5])
    volume = sys.argv[5] if len(sys.argv) == 6 else "PSXSPLASH"

    files = []
    scene = 0
    while os.path.exists(os.path.join(export, f"scene_{scene}.splashpack")):
        for kind in KINDS:
            name = f"scene_{scene}.{kind}"
            if os.path.exists(os.path.join(export, name)):
                files.append({"path": os.path.join(export, name), "name": name})
        scene += 1
    if scene == 0:
        sys.exit(f"mkdisc: no scene_0.splashpack in {export}")

    index = {"executable": exe, "files": files, "pvd": {"volume_id": volume}}
    with tempfile.TemporaryDirectory() as tmp:
        spec = os.path.join(tmp, "disc.json")
        with open(spec, "w") as f:
            json.dump(index, f, indent=2)
        run = subprocess.run([authoring, spec, "-o", out, "-basedir", "/"], capture_output=True, text=True)
    if run.returncode != 0:
        sys.stderr.write(run.stdout + run.stderr)
        sys.exit(f"mkdisc: authoring exited with {run.returncode}")
    margin = [l for l in run.stdout.splitlines() if l.startswith("Maximum margin:")]
    # The archive header keeps the decompression margin in one byte of 16-byte
    # blocks (plus one), so anything from 4096 bytes up is silently cut short.
    if margin and int(margin[0].split(":")[1]) >= 4096:
        sys.exit(f"mkdisc: {margin[0]}, more than the archive header can hold")
    print(f"mkdisc: {scene} scene(s), {len(files)} files -> {out}")


if __name__ == "__main__":
    main()
