# Boot test

`boot.sh` runs a PCdrv build of the engine in headless pcsx-redux on the scene in `fixture/` and
passes once the scene's Lua script prints `BOOT OK: 60 frames`, which it does from `onUpdate` on
the 60th frame. CI runs it on every push and pull request.

    make LOADER=pcdrv
    make -C third_party/nugget/openbios
    tests/boot/fetch-redux.sh /tmp/redux
    tests/boot/boot.sh /tmp/redux/squashfs-root/AppRun third_party/nugget/openbios/openbios.bin psxsplash.ps-exe

`fetch-redux.sh` downloads the newest Linux dev build of pcsx-redux and unpacks the AppImage, so
no FUSE is needed. Any other `pcsx-redux` binary works as the first argument.

## Regenerating the fixture

`scene/` is the source: the courtyard example from splashedit-ng with the boot script attached to
one crate. The fixture is its export by `splashpack-cli`. Re-export it whenever the engine stops
reading the committed version (the job then fails with the engine's own version message):

    cmake -S <splashedit-ng> -B build -DSPLASHEDIT_BUILD_EDITOR=OFF
    cmake --build build --target splashpack-cli
    build/cli/splashpack-cli export tests/boot/scene/boot.scene -o tests/boot/fixture/scene_0.splashpack

The export writes `scene_0.vram` and `scene_0.spu` beside the `.splashpack`. Commit all three.
