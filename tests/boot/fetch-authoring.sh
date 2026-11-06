#!/bin/bash
# Build PCSX-Redux's `authoring` tool, which writes the archive engine's disc image.
# usage: tests/boot/fetch-authoring.sh <dir>   (the tool ends up at <dir>/authoring)
# Only the sources the tool needs are compiled: the full Makefile wants every submodule.
set -eu
dir=$1
ref=${AUTHORING_REF:-598ddd7d454e9365c2d5be30e26fb67e5946a7b1}
mkdir -p "$dir"
cd "$dir"
git init -q src
cd src
git fetch -q --depth 1 https://github.com/pcsx-redux/pcsx-redux "$ref"
git checkout -q FETCH_HEAD
for m in fmt ELFIO magic_enum ucl expected iec-60908b args; do
  git submodule update -q --init --depth 1 "third_party/$m"
done
git submodule update -q --init --depth 1 src/mips
inc="-I. -Isrc -Ithird_party -Ithird_party/ELFIO -Ithird_party/magic_enum/include -Ithird_party/fmt/include
  -Ithird_party/ucl -Ithird_party/ucl/include -Ithird_party/expected/include"
mkdir -p ../objs
for f in third_party/ucl/src/{n2e_99,alloc,n2e_ds}.c third_party/iec-60908b/{correct,edcecc,tables}.c; do
  cc -O2 $inc -c "$f" -o "../objs/$(basename "$f").o"
done
for f in src/support/{container-file,cpu-features,file,md5,mem4g,zfile}.cc \
         src/supportpsx/{adpcm,binloader,dct,iec-60908b,iso9660-builder,ps1-packer,psyq-obj,sf2,spu-sample-layout,spusample,ucl-utils}.cc \
         third_party/fmt/src/{os,format}.cc tools/authoring/authoring.cc; do
  g++ -std=c++2b -O2 -pthread $inc -c "$f" -o "../objs/$(basename "$f").o"
done
g++ -o ../authoring ../objs/*.o -pthread -lz
# -h prints the usage and exits non-zero.
../authoring -h | grep -q 'Usage:'
