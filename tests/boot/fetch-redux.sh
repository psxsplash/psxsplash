#!/bin/bash
# Download the newest Linux x64 dev build of pcsx-redux and unpack it without FUSE.
# usage: tests/boot/fetch-redux.sh <dir>   (the emulator ends up at <dir>/squashfs-root/AppRun)
set -eu
dir=$1
base=https://distrib.app/storage/manifests/pcsx-redux/dev-linux-x64
mkdir -p "$dir"
cd "$dir"
id=$(curl -fsSL "$base/manifest.json" | python3 -c 'import json, sys; print(max(b["id"] for b in json.load(sys.stdin)["builds"]))')
path=$(curl -fsSL "$base/manifest-$id.json" | python3 -c 'import json, sys; print(json.load(sys.stdin)["path"])')
echo "pcsx-redux dev build $id: $path"
curl -fsSL -o redux.zip "https://distrib.app$path"
unzip -q -o redux.zip
chmod +x ./*.AppImage
./*.AppImage --appimage-extract > /dev/null
test -x squashfs-root/AppRun
