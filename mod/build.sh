#!/bin/sh
set -eu
if [ $# -ne 1 ]; then echo "usage: build.sh <version>" >&2; exit 2; fi
VERSION="$1"
MOD_ID="local@dynamic-island-openmeteo"
HERE="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$HERE/dynamic-island-openmeteo.wh.cpp"
OUT="$HERE/out"
COMPILER="/c/Program Files/Windhawk/Compiler"
ENGINE="C:/Program Files/Windhawk/Engine/1.7.3"
LIBS="-lole32 -loleaut32 -lshcore -ld2d1 -ldwrite -ldwmapi -lgdi32 -luser32 -lshell32 -lruntimeobject -lwindowscodecs -lsetupapi -lwinhttp -lpdh -lwinmm"

sed -i "s|^// @version .*|// @version         $VERSION|" "$SOURCE"
SUFFIX=$(printf '%06d' $((RANDOM * RANDOM % 1000000)))
NAME="${MOD_ID}_${VERSION}_${SUFFIX}.dll"
mkdir -p "$OUT/64" "$OUT/32"
echo "$NAME" > "$OUT/name.txt"
echo "$VERSION" > "$OUT/ver.txt"

cd "$COMPILER"
STATUS=0
for pair in "64 x86_64-w64-mingw32" "32 i686-w64-mingw32"; do
  set -- $pair
  ./bin/clang++.exe -std=c++23 -O2 -shared -DUNICODE -D_UNICODE \
    -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 \
    -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD "-DWH_MOD_ID=L\"$MOD_ID\"" "-DWH_MOD_VERSION=L\"$VERSION\"" \
    "$ENGINE/$1/windhawk.lib" -x c++ - -include windhawk_api.h -target "$2" \
    -Wl,--export-all-symbols -o "$OUT/$1/$NAME" $LIBS < "$SOURCE" 2>&1 | grep -E "error" | head || true
  if [ -f "$OUT/$1/$NAME" ]; then echo "$1-bit: ok"; else echo "$1-bit: FAILED"; STATUS=1; fi
done
exit $STATUS
