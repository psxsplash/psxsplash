#!/bin/bash
# Boot psxsplash on a committed scene in headless pcsx-redux and wait for the
# line its Lua script prints when it passes (or "BOOT FAIL" when it fails).
# usage: tests/boot/boot.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> [seconds] [fixture dir] [token]
set -u
redux=$1; bios=$2; exe=$3; limit=${4:-120}
here=$(cd "$(dirname "$0")" && pwd)
fixture=${5:-$here/fixture}
token=${6:-'BOOT OK: 60 frames'}
pid=
work=$(mktemp -d)
# BOOT_LOG=<file> keeps the emulator log.
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; [ -n "${BOOT_LOG:-}" ] && cp "$work/log.txt" "$BOOT_LOG"; rm -rf "$work"' EXIT
cp "$fixture"/scene_0.* "$work/" || exit 1
version=$(od -An -tu2 -j2 -N2 "$work/scene_0.splashpack" | tr -d ' ')
echo "fixture: splashpack v$version"

"$redux" -cli -stdout -lua_stdout -bios "$bios" -pcdrv -pcdrvbase "$work" -loadexe "$exe" -run -fastboot \
  > "$work/log.txt" 2>&1 &
pid=$!
for _ in $(seq "$limit"); do
  if grep -qF "$token" "$work/log.txt"; then
    echo "pass: '$token' seen"
    exit 0
  fi
  grep -q 'BOOT FAIL' "$work/log.txt" && break
  kill -0 $pid 2>/dev/null || break
  sleep 1
done
if grep -q 'BOOT FAIL' "$work/log.txt"; then
  echo "fail: $(grep -m1 'BOOT FAIL' "$work/log.txt")" >&2
elif kill -0 $pid 2>/dev/null; then
  echo "fail: '$token' not printed within ${limit}s" >&2
else
  echo "fail: the emulator exited before printing '$token'" >&2
fi
echo "--- emulator log, pad polling removed ---" >&2
grep -v '^Unknown command for pad' "$work/log.txt" | tail -n 100 >&2
exit 1
