#!/bin/bash
# Boot psxsplash on a committed scene in headless pcsx-redux and wait for the
# line its Lua script prints when it passes (or "BOOT FAIL" when it fails).
# The fixture dir is packed into an image with tools/mkdisc.py and read over
# PCdrv as psxsplash.bin with no disc, the way SplashEdit's Play runs it.
# usage: tests/boot/boot.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> <authoring> [seconds] [fixture dir] [token]
set -u
redux=$1; bios=$2; exe=$(realpath "$3"); authoring=$(realpath "$4"); limit=${5:-120}
here=$(cd "$(dirname "$0")" && pwd)
fixture=${6:-$here/fixture}
token=${7:-'BOOT OK: 60 frames'}
pid=
work=$(mktemp -d)
# BOOT_LOG=<file> keeps the emulator log. BOOT_DISC=1 boots the image as a disc (-iso) in place of
# PCdrv, as movies need.
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; [ -n "${BOOT_LOG:-}" ] && cp "$work/log.txt" "$BOOT_LOG"; rm -rf "$work"' EXIT
version=$(od -An -tu2 -j2 -N2 "$fixture/scene_0.splashpack" | tr -d ' ')
echo "fixture: splashpack v$version"
mkdir "$work/img"
python3 "$here/../../tools/mkdisc.py" "$authoring" "$exe" "$fixture" "$work/img/psxsplash.bin" || exit 1

media=(-pcdrv -pcdrvbase "$work/img")
via="via PCdrv psxsplash.bin"
if [ -n "${BOOT_DISC:-}" ]; then
  media=(-iso "$work/img/psxsplash.bin")
  via="via the CD drive"
fi
"$redux" -cli -stdout -lua_stdout -bios "$bios" "${media[@]}" -loadexe "$exe" -run -fastboot \
  > "$work/log.txt" 2>&1 &
pid=$!
for _ in $(seq "$limit"); do
  if grep -qF "$token" "$work/log.txt"; then
    if grep -qF "FileLoader: archive of" "$work/log.txt" && grep -qF "$via" "$work/log.txt"; then
      echo "pass: '$token' seen, read $via"
      exit 0
    fi
    seen=1
    break
  fi
  grep -q 'BOOT FAIL' "$work/log.txt" && break
  kill -0 $pid 2>/dev/null || break
  sleep 1
done
if [ -n "${seen:-}" ]; then
  echo "fail: '$token' seen, but the archive was not read $via" >&2
elif grep -q 'BOOT FAIL' "$work/log.txt"; then
  echo "fail: $(grep -m1 'BOOT FAIL' "$work/log.txt")" >&2
elif kill -0 $pid 2>/dev/null; then
  echo "fail: '$token' not printed within ${limit}s" >&2
else
  echo "fail: the emulator exited before printing '$token'" >&2
fi
echo "--- emulator log, pad polling removed ---" >&2
grep -v '^Unknown command for pad' "$work/log.txt" | tail -n 100 >&2
exit 1
