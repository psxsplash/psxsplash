# psxsplash

The PlayStation 1 runtime engine for [SplashEdit](https://github.com/psxsplash/splashedit). Built on [PSYQo](https://github.com/grumpycoders/pcsx-redux/tree/main/src/mips/psyqo).

psxsplash reads scene data exported by SplashEdit and runs your game on real PS1 hardware or in an emulator. It handles rendering, collision, navigation, Lua scripting, UI, audio, cutscenes, animations, and multi-scene management.

## Documentation

Full documentation, tutorials, and Lua API reference at **[psxsplash.github.io/docs](https://psxsplash.github.io/docs/)**.

## How It Works

You don't interact with this repo directly. SplashEdit downloads and compiles it for you through the Unity Control Panel. If you want to contribute or dig into the source, read on.

## Building

Requires a MIPS cross-compiler (`mipsel-none-elf` on Windows, `mipsel-linux-gnu` on Linux) and GNU Make.

```bash
make all -j$(nproc)
```

Build options:

| Flag | Description |
|------|-------------|
| `LOADER=cdrom` | CD-ROM backend (real hardware) |
| `PCDRV_SUPPORT=1` | PCdrv backend (emulator, default) |
| `NOPARSER=1` | Strip Lua parser, use precompiled bytecode |
| `PSXSPLASH_MEMOVERLAY=1` | Runtime memory usage overlay |
| `PSXSPLASH_FPSOVERLAY=1` | FPS counter overlay |
| `OT_SIZE=N` | Ordering table size |
| `BUMP_SIZE=N` | Bump allocator size |

## Contributing

Pull requests are welcome. See the [contributing guide](https://psxsplash.github.io/docs/reference/contributing/) for areas where help is needed.


