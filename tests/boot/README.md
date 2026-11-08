# Boot test

`boot.sh` runs the engine in headless pcsx-redux on the scene in `fixture/` and passes once the
scene's Lua script prints `BOOT OK: 60 frames`, which it does from `onUpdate` on the 60th frame,
and the engine says it read its archive over PCdrv. The fixture is packed into an image with
`tools/mkdisc.py` and PCSX-Redux's `authoring` tool, and the engine reads it as `psxsplash.bin`
over PCdrv with no disc, the way SplashEdit's Play runs it. CI runs it on every push and pull
request.

    make
    make -C third_party/nugget/openbios
    tests/boot/fetch-redux.sh /tmp/redux
    tests/boot/fetch-authoring.sh /tmp/authoring
    tests/boot/boot.sh /tmp/redux/squashfs-root/AppRun third_party/nugget/openbios/openbios.bin psxsplash.ps-exe /tmp/authoring/authoring

A second fixture, `fixture-cutscene/`, plays two cutscenes chained through `onComplete` with keys on
their last frame, under frames slow enough that a tick never lands on that frame:

    tests/boot/boot.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe <authoring> 300 tests/boot/fixture-cutscene 'BOOT OK: cutscenes'

`luadebug.sh` drives the Lua debugger (`src/luadebug.hh`) the way SplashEdit's Play does, on
`fixture-luadebug/`: its `luabp.txt`, a host file beside `psxsplash.bin`, sets a breakpoint on line
11 of the boot script; the run must stop there with the local `answer` = 42, ignore a resume meant
for another stop, step to line 12 with the global `marker` = 43, then continue to
`BOOT OK: luadebug 43`. With `NOBP=1` it runs without `luabp.txt`: no debugger, no stop, the same
pass line.

    tests/boot/luadebug.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe <authoring> 120

A third, `fixture-particles/`, has two particle emitters (splashpack v32): its scene script finds
them by name, checks which one autoplays and bursts the other; the crate's script stops the playing
one from Lua at update 240 and passes once its particles have all lived out their lifetime:

    tests/boot/boot.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe <authoring> 300 tests/boot/fixture-particles 'BOOT OK: particles'

A fourth, `fixture-movie/`, plays `CLIP` with `Movie.Play`, then `TV` on a crate with
`Movie.PlayOn`, and prints `BOOT OK: movies`. Movies stream off the drive and never over PCdrv, and
the engine finds `MOVIES/CLIP.STR` as a stream entry of the archive (raw Form 2 sectors, so its
audio stays interleaved). `authoring` writes no stream entries, so `mkdisc.py` leaves `.STR` files
out and CI does not boot this fixture; it gets a CI run once Redux's authoring tool encodes movies
itself. Booted on such a disc (`BOOT_DISC=1` boots the image with `-iso`), the check also greps
the engine's own count for each clip, so a player that runs and shows nothing fails it:

    grep -F 'Movie CLIP: shown 12 dropped 0' movie.log && grep -F 'Movie TV: ended, 58 of 58' movie.log

`boot-archive.sh` boots `fixture/` from the drive (`-iso`) and over PCdrv, and checks which way the
engine read it:

    tests/boot/boot-archive.sh <pcsx-redux> <openbios.bin> psxsplash.ps-exe <authoring> 120

Both clips are ffmpeg's test pattern, made with psxavenc: `CLIP.STR` one second of `testsrc2` at 160x112 and 15 fps
with a 440 Hz tone (`-t str -v v2 -f 37800 -b 4 -c 2 -s 160x112 -r 15`, 12 frames), `TV.SBS` four seconds at 64x64
(`-t sbs -v v2 -s 64x64 -a 1024`, 58 frames). The scene is `scene/movie.scene`, the boot scene with the crate on a
16-bit texture for `Movie.PlayOn`:

    build/cli/splashpack-cli export tests/boot/scene/movie.scene -o tests/boot/fixture-movie/scene_0.splashpack

`fetch-redux.sh` downloads the newest Linux dev build of pcsx-redux and unpacks the AppImage, so
no FUSE is needed. Any other `pcsx-redux` binary works as the first argument. `fetch-authoring.sh`
builds the `authoring` tool from a pinned pcsx-redux commit.

## Regenerating the fixture

`scene/` is the source: the courtyard example from splashedit-ng with the boot script attached to
one crate. The fixture is its export by `splashpack-cli`. Re-export it whenever the engine stops
reading the committed version (the job then fails with the engine's own version message):

    cmake -S <splashedit-ng> -B build -DSPLASHEDIT_BUILD_EDITOR=OFF
    cmake --build build --target splashpack-cli
    build/cli/splashpack-cli export tests/boot/scene/boot.scene -o tests/boot/fixture/scene_0.splashpack

    build/cli/splashpack-cli export tests/boot/scene/cutscene.scene -o tests/boot/fixture-cutscene/scene_0.splashpack

    build/cli/splashpack-cli export tests/boot/scene/luadebug.scene -o tests/boot/fixture-luadebug/scene_0.splashpack --lua-debug

`--lua-debug` keeps the line numbers and local names the debugger needs (Play does the same while
a project has breakpoints); `fixture-luadebug/luabp.txt` is written by hand.
    build/cli/splashpack-cli export tests/boot/scene/particles.scene -o tests/boot/fixture-particles/scene_0.splashpack

The export writes `scene_0.vram` and `scene_0.spu` beside the `.splashpack`. Commit all three.
