#!/bin/bash
# Boot the engine on the test scene both ways it can read its image: from the
# CD drive, and over PCdrv with no disc.
# usage: tests/boot/boot-archive.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> <authoring> [seconds]
set -u
redux=$1; bios=$2; exe=$(realpath "$3"); authoring=$(realpath "$4"); limit=${5:-120}
here=$(cd "$(dirname "$0")" && pwd)
token='BOOT OK: 60 frames'
work=$(mktemp -d)
pid=
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; rm -rf "$work"' EXIT
mkdir "$work/img"
python3 "$here/../../tools/mkdisc.py" "$authoring" "$exe" "$here/fixture" "$work/img/psxsplash.bin" || exit 1

# arm <name> <line the loader must print> <emulator arguments...>
arm() {
  local name=$1 via=$2; shift 2
  "$redux" -cli -stdout -lua_stdout -bios "$bios" "$@" -loadexe "$exe" -run -fastboot > "$work/$name.txt" 2>&1 &
  pid=$!
  local ok=
  for _ in $(seq "$limit"); do
    if grep -qF "$token" "$work/$name.txt"; then ok=1; break; fi
    grep -q 'BOOT FAIL' "$work/$name.txt" && break
    kill -0 $pid 2>/dev/null || break
    sleep 1
  done
  kill $pid 2>/dev/null; wait $pid 2>/dev/null; pid=
  if [ -n "$ok" ] && grep -qF "$via" "$work/$name.txt"; then
    echo "pass: $name: '$via', '$token'"
    return 0
  fi
  echo "fail: $name: wanted '$via' and '$token'" >&2
  grep -v '^Unknown command for pad' "$work/$name.txt" | tail -n 60 >&2
  return 1
}

status=0
arm drive "FileLoader: archive of" -iso "$work/img/psxsplash.bin" || status=1
grep -qF "via the CD drive" "$work/drive.txt" || { echo "fail: drive: not read from the drive" >&2; status=1; }
arm pcdrv "via PCdrv psxsplash.bin" -pcdrv -pcdrvbase "$work/img" || status=1
exit $status
