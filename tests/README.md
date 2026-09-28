# psxsplash host tests

Unit tests for the networking stack that run on your **PC** — no MIPS toolchain,
no emulator, no PlayStation.

`netlink.hh` already promised this was possible:

> It is transport-agnostic and hardware-free (depends only on INetTransport and
> the pure protocol header), so it is unit-testable on a host with a mock
> transport.

This is that test. The **real** `src/netlink.cpp` is compiled with the host
compiler and driven through a mock transport, so these tests exercise shipping
code rather than a copy of it.

## Layout

| Path | What it is |
|---|---|
| `host/test_netlink.cpp` | The unit tests (framing, CRC, resync, reliability). |
| `host/gen_vectors.cpp` | Generates the cross-language frame corpus. |
| `host/mocktransport.hh` | `MockTransport` (fault injection, write throttling) + `RecordingHandler`. |
| `host/testing.hh` | ~50-line test framework. No third-party dependency, so this builds anywhere the engine does. |
| `host/compat.hh` | Maps `__builtin_memcpy`/`__builtin_memcmp` for MSVC. Test-only — engine sources are never modified for the host build. |
| `vectors/` | The generated corpus (committed). |

## Running

**Linux / macOS / Docker / anywhere with GCC or Clang:**

```sh
cd psxsplash/tests/host
make            # build + run the tests
make vectors    # regenerate the corpus
```

**Windows (MSVC — the stock dev box has no GCC):**

```powershell
cd psxsplash\tests\host
.\build_msvc.ps1            # build + run the tests
.\build_msvc.ps1 -Vectors   # regenerate the corpus
```

## The vector corpus

`vectors/` holds raw on-wire frames plus a `manifest.json` describing what each
one decodes to. The C++ engine is the **reference implementation** of the PSNL
wire format, so it generates the corpus by driving the real `NetLink` and
capturing its output — never by hand-assembling bytes, which would let the corpus
drift from the code it exists to pin down.

The Python `psnl` package (`packages/psnl/`) replays the same corpus through
encode **and** decode. That cross-language conformance is what stops the console
and the central server from growing two subtly different protocols:

```sh
cd packages/psnl && python -m pytest tests -q
```

**A diff in `vectors/` is a deliberate protocol change.** Review it as one. If a
conformance test fails, fix the code — never the expected bytes.

## Notes

- `crc16_canonical_check_value` asserts `crc16("123456789") == 0x29B1`, the
  published check value for CRC-16/CCITT-FALSE. The Python suite asserts the
  same constant. This pins both sides to a *named, published* algorithm rather
  than merely to each other — if both drifted the same way, this still catches it.
- `src/nettest.cpp` (the `NETTEST=1` on-device self-test) keeps its own minimal
  `PipeEnd`/`TestHandler` on purpose, so the shipped build never links test
  scaffolding. `mocktransport.hh` is the richer host-only equivalent.
