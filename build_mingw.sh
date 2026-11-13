#!/bin/sh
# 64-bit Windows exe with MinGW-w64 (Linux cross-compile, or MSYS2 on Windows).
set -e
CXX=${CXX:-x86_64-w64-mingw32-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++"
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
$WINDRES noiseflip.rc -O coff -o noiseflip_res.o
$CXX $FLAGS noiseflip.cpp noiseflip_res.o -o noiseflip.exe -lwinmm -lws2_32 -lshell32 -luser32
$CXX $FLAGS fake_skate.cpp -o fake_skate.exe
# the tests run on any machine, no Windows needed:
T="${TMPDIR:-/tmp}/noiseflip_tests"
g++ -std=c++17 -O1 -Wall -Wextra tests.cpp -o "$T" && "$T"
