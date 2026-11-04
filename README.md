# psxsplash

The PlayStation 1 runtime engine for [SplashEdit](https://github.com/psxsplash/splashedit). Built on [PSYQo](https://github.com/grumpycoders/pcsx-redux/tree/main/src/mips/psyqo).

psxsplash reads scene data exported by SplashEdit and runs your game on real PS1 hardware or in an emulator. It handles rendering, collision, navigation, Lua scripting, UI, audio, cutscenes, animations, and multi-scene management.

## Documentation

Full documentation, tutorials, and Lua API reference at **[psxsplash.github.io/docs](https://psxsplash.github.io/docs/)**.

## How It Works

You don't interact with this repo directly. SplashEdit downloads and compiles it for you through the Unity Control Panel. If you want to contribute or dig into the source, read on.

## Building

Requires a MIPS cross-compiler (`mipsel-none-elf` on Windows, `mipsel-linux-gnu` on Linux) and [xmake](https://xmake.io).

```bash
xmake
```

This builds every engine into `build/psx/mipsel/release/`, each under its own name:

| File | Use |
|------|-----|
| `psxsplash.ps-exe` | Loads scenes over PCdrv: pcsx-redux, or a console over a serial cable |
| `psxsplash-cdrom.ps-exe` | Loads scenes from the CD: disc images |
| `psxsplash-perf.ps-exe` | PCdrv, with the FPS counter and heap usage overlay |
| `psxsplash-rooms.ps-exe` | PCdrv, with the room debug overlay |
| `psxsplash-perf-rooms.ps-exe` | PCdrv, with both |

`xmake build psxsplash-cdrom.ps-exe` builds one of them. Options, set with `xmake f` before building and applied to every engine:

| Option | Description |
|--------|-------------|
| `--features=...` | Subsystems to compile in, comma separated, from `bootlogo,net,ui,sprites,skin,cutscene,lights,streaming,memcard,nav,agents,collision,shadows` (`bootlogo` is the psxsplash logo animation shown before the first scene). Unset builds all of them; `none` builds the smallest engine. Without `bootlogo`, every file name ends in `-nologo`. |
| `--noparser=y` | Strip the Lua parser; scripts must be precompiled bytecode |
| `--ot_size=N` | Ordering table size when a scene does not set one |
| `--bump_size=N` | Bump allocator size when a scene does not set one |
| `--profiler=y` | Per-frame profiler overlay and PCSX variable export |

The Makefile still builds one engine at a time (`make LOADER=cdrom`, `MEMOVERLAY=1`, `FPSOVERLAY=1`, `ROOMDEBUG=1`, `NOPARSER=1`, `FEATURES=...`), always as `psxsplash.ps-exe`, so each build replaces the last. CI builds with both.

## Contributing

Pull requests are welcome. See the [contributing guide](https://psxsplash.github.io/docs/latest/reference/contributing/) for areas where help is needed.


