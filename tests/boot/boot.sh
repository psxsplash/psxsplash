#!/bin/bash
# Boot psxsplash on the committed scene in headless pcsx-redux and wait for the
# line its Lua script prints after 60 frames of play.
# usage: tests/boot/boot.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> [seconds]
set -u
redux=$1; bios=$2; exe=$3; limit=${4:-120}
here=$(cd "$(dirname "$0")" && pwd)
token='BOOT OK: 60 frames'
pid=
work=$(mktemp -d)
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$work"' EXIT
cp "$here"/fixture/scene_0.* "$work/" || exit 1
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
  kill -0 $pid 2>/dev/null || break
  sleep 1
done
if kill -0 $pid 2>/dev/null; then
  echo "fail: '$token' not printed within ${limit}s" >&2
else
  echo "fail: the emulator exited before printing '$token'" >&2
fi
echo "--- emulator log, pad polling removed ---" >&2
grep -v '^Unknown command for pad' "$work/log.txt" | tail -n 100 >&2
exit 1
