#!/bin/sh
# Builds patch/build/winmm.dll (32-bit) with llvm-mingw.
#   LLVM_MINGW=/path/to/llvm-mingw sh patch/build.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="${LLVM_MINGW:-/d/Projects/Tools/llvm-mingw-20261006-ucrt-x86_64}"
CC="$TOOLCHAIN/bin/i686-w64-mingw32-clang"
mkdir -p "$HERE/build"
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-missing-field-initializers \
    -Wno-sign-compare -Wno-unused-function -Wno-unused-variable \
    -shared -o "$HERE/build/winmm.dll" \
    "$HERE/src/proxy.c" "$HERE/src/hooks.c" "$HERE/src/cdaudio.c" "$HERE/src/vfs.c" "$HERE/src/ddwin.c" "$HERE/src/glidewin.c" "$HERE/src/devui.c" "$HERE/src/soundlog.c" "$HERE/src/glidetrace.c" "$HERE/src/input.c" "$HERE/src/prof.c" "$HERE/src/texcache.c" "$HERE/src/ap.c" "$HERE/src/apdlg.c" "$HERE/src/traps.c" \
    "$HERE/src/gen/winmm_stubs.S" "$HERE/src/gen/winmm.def" \
    -static -lgdi32 -luser32 -lws2_32 -luuid -ldxguid \
    -Wl,--enable-stdcall-fixup
echo "built $HERE/build/winmm.dll"
