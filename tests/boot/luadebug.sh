#!/bin/bash
# Lua breakpoints in headless pcsx-redux, driven the way SplashEdit's Play
# drives them (src/luadebug.hh): fixture-luadebug/luabp.txt sets a breakpoint
# on line 11 of scripts/luadebug_check.lua. The run must stop there with the
# local `answer` = 42, step to line 12 with the global `marker` = 43, then
# continue to "BOOT OK: luadebug 43".
#
# With NOBP=1 it runs without luabp.txt: no debugger, no stop, the pass line
# all the same.
# usage: tests/boot/luadebug.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> [seconds per step]
set -u
redux=$1; bios=$2; exe=$3; limit=${4:-120}
here=$(cd "$(dirname "$0")" && pwd)
fixture=$here/fixture-luadebug
pid=
work=$(mktemp -d)
log=$work/log.txt
# BOOT_LOG=<file> keeps the emulator log.
trap '[ -n "$pid" ] && kill $pid 2>/dev/null; [ -n "${BOOT_LOG:-}" ] && cp "$log" "$BOOT_LOG"; rm -rf "$work"' EXIT
cp "$fixture"/scene_0.* "$work/" || exit 1
[ -n "${NOBP:-}" ] || cp "$fixture/luabp.txt" "$work/" || exit 1

fail() {
  echo "fail: $*" >&2
  echo "--- emulator log, pad polling removed ---" >&2
  grep -v '^Unknown command for pad' "$log" | tail -n 60 >&2
  exit 1
}
# Waits for a line containing $1.
waitfor() {
  for _ in $(seq "$limit"); do
    grep -qF "$1" "$log" && return 0
    kill -0 $pid 2>/dev/null || fail "the emulator exited before '$1'"
    sleep 1
  done
  fail "'$1' not printed within ${limit}s"
}
expect() { grep -qF "$1" "$log" || fail "no line '$1'"; echo "ok: $1"; }
absent() { ! grep -qF "$1" "$log" || fail "unexpected line '$1'"; echo "ok: no '$1'"; }

"$redux" -cli -stdout -lua_stdout -bios "$bios" -pcdrv -pcdrvbase "$work" -loadexe "$exe" -run -fastboot \
  > "$log" 2>&1 &
pid=$!

if [ -n "${NOBP:-}" ]; then
  waitfor 'BOOT OK: luadebug 43'
  absent 'psxsplash: lua debugger on'
  absent 'psxsplash: lua break'
  echo "pass: no breakpoints, no stop"
  exit 0
fi

waitfor 'psxsplash: lua break end 1'
expect 'psxsplash: lua debugger on, 1 breakpoints'
expect 'psxsplash: lua break 1 11 scripts/luadebug_check.lua'
expect 'psxsplash: lua local answer number 42'
expect 'psxsplash: lua local self table'
expect 'psxsplash: lua upvalue frames number 30'
absent 'psxsplash: lua global marker'
# The engine's API tables and the VM's own names are not game state.
absent 'psxsplash: lua global _G.Input'
absent 'psxsplash: lua global _G._VERSION'
# Stopped: nothing past the breakpoint runs while the editor has not answered.
sleep 3
absent 'BOOT OK: luadebug'
absent 'psxsplash: lua resume 1'

# A command for another stop is ignored.
echo '7 continue' > "$work/luabp.cmd"
sleep 3
absent 'psxsplash: lua resume 1'

echo '1 step' > "$work/luabp.cmd"
waitfor 'psxsplash: lua break end 2'
expect 'psxsplash: lua resume 1 step'
expect 'psxsplash: lua break 2 12 scripts/luadebug_check.lua'
expect 'psxsplash: lua local answer number 42'
expect 'psxsplash: lua global marker number 43'
absent 'BOOT OK: luadebug'

echo '2 continue' > "$work/luabp.cmd"
waitfor 'BOOT OK: luadebug 43'
expect 'psxsplash: lua resume 2 continue'
sleep 2
absent 'psxsplash: lua break 3'
echo "pass: stopped at 11, stepped to 12, continued"
exit 0
