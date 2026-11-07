# Boot test

`boot.sh` runs a PCdrv build of the engine in headless pcsx-redux on the scene in `fixture/` and
passes once the scene's Lua script prints `BOOT OK: 60 frames`, which it does from `onUpdate` on
the 60th frame. CI runs it on every push and pull request.

    make LOADER=pcdrv
    make -C third_party/nugget/openbios
    tests/boot/fetch-redux.sh /tmp/redux
    tests/boot/boot.sh /tmp/redux/squashfs-root/AppRun third_party/nugget/openbios/openbios.bin psxsplash.ps-exe

A second fixture, `fixture-cutscene/`, plays two cutscenes chained through `onComplete` with keys on
their last frame, under frames slow enough that a tick never lands on that frame:

    tests/boot/boot.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe 300 tests/boot/fixture-cutscene 'BOOT OK: cutscenes'

A third, `fixture-particles/`, has two particle emitters (splashpack v32): its scene script finds
them by name, checks which one autoplays and bursts the other; the crate's script stops the playing
one from Lua at update 240 and passes once its particles have all lived out their lifetime:

    tests/boot/boot.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe 300 tests/boot/fixture-particles 'BOOT OK: particles'

A fourth, `fixture-movie/`, needs a cdrom build, since movies are disc-only. `mkdisc.py` puts the scene and two
clips on a Mode 2 disc (ISO9660, Form 1 sectors with their EDC and ECC, a `.STR` copied sector for sector so its
audio stays Form 2), and `BOOT_ISO` boots it in place of PCdrv. The script plays `CLIP` with `Movie.Play`, then
`TV` on a crate with `Movie.PlayOn`; the check also greps the engine's own count for each, so a player that runs and
shows nothing fails it:

    make clean && make LOADER=cdrom
    f=tests/boot/fixture-movie
    tests/boot/mkdisc.py /tmp/movie.bin SCENE0/SCENE_0.SPK=$f/scene_0.splashpack SCENE0/SCENE_0.VRM=$f/scene_0.vram \
        SCENE0/SCENE_0.SPU=$f/scene_0.spu MOVIES/CLIP.STR=$f/CLIP.STR MOVIES/TV.SBS=$f/TV.SBS
    BOOT_ISO=/tmp/movie.cue BOOT_LOG=/tmp/movie.log tests/boot/boot.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe 300 $f 'BOOT OK: movies'
    grep -F 'Movie CLIP: shown 12 dropped 0' /tmp/movie.log && grep -F 'Movie TV: ended, 58 of 58' /tmp/movie.log

Both clips are ffmpeg's test pattern, made with psxavenc: `CLIP.STR` one second of `testsrc2` at 160x112 and 15 fps
with a 440 Hz tone (`-t str -v v2 -f 37800 -b 4 -c 2 -s 160x112 -r 15`, 12 frames), `TV.SBS` four seconds at 64x64
(`-t sbs -v v2 -s 64x64 -a 1024`, 58 frames). The scene is `scene/movie.scene`, the boot scene with the crate on a
16-bit texture for `Movie.PlayOn`:

    build/cli/splashpack-cli export tests/boot/scene/movie.scene -o tests/boot/fixture-movie/scene_0.splashpack

`fetch-redux.sh` downloads the newest Linux dev build of pcsx-redux and unpacks the AppImage, so
no FUSE is needed. Any other `pcsx-redux` binary works as the first argument.

## Regenerating the fixture

`scene/` is the source: the courtyard example from splashedit-ng with the boot script attached to
one crate. The fixture is its export by `splashpack-cli`. Re-export it whenever the engine stops
reading the committed version (the job then fails with the engine's own version message):

    cmake -S <splashedit-ng> -B build -DSPLASHEDIT_BUILD_EDITOR=OFF
    cmake --build build --target splashpack-cli
    build/cli/splashpack-cli export tests/boot/scene/boot.scene -o tests/boot/fixture/scene_0.splashpack

    build/cli/splashpack-cli export tests/boot/scene/cutscene.scene -o tests/boot/fixture-cutscene/scene_0.splashpack

    build/cli/splashpack-cli export tests/boot/scene/particles.scene -o tests/boot/fixture-particles/scene_0.splashpack

The export writes `scene_0.vram` and `scene_0.spu` beside the `.splashpack`. Commit all three.
