#!/bin/sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out/tests"
SOURCE="$HERE/dynamic-island-openmeteo.wh.cpp"
COMPILER="${MOD_TEST_COMPILER:-windhawk}"
mkdir -p "$OUT"
awk '/^namespace notification_center$/,/^struct VolumeSnapshot \{/' "$SOURCE" | sed '$d' > "$OUT/notification_center_under_test.h"
{
  echo "#include \"island_pipe_stubs.h\""
  awk '/^namespace island_pipe$/,/^bool StartThreads\(\) \{/' "$SOURCE" | sed '$d'
} > "$OUT/island_pipe_under_test.h"

build_and_run() {
  name="$1"
  if [ "$COMPILER" = "msvc" ]; then
    MSYS2_ARG_CONV_EXCL='*' cl.exe /nologo /std:c++latest /utf-8 /EHsc /W4 /WX /O2 /DUNICODE /D_UNICODE \
      /I "$(cygpath -w "$OUT")" /I "$(cygpath -w "$HERE/tests")" "$(cygpath -w "$HERE/tests/$name.cpp")" \
      /Fo"$(cygpath -w "$OUT")\\" /Fe"$(cygpath -w "$OUT/$name.exe")" /link advapi32.lib
  else
    "/c/Program Files/Windhawk/Compiler/bin/clang++.exe" -std=c++23 -O2 -Wall -Wextra -Werror -static -DUNICODE -D_UNICODE -target x86_64-w64-mingw32 \
      -I "$OUT" -I "$HERE/tests" "$HERE/tests/$name.cpp" -o "$OUT/$name.exe" -ladvapi32
  fi
  "$OUT/$name.exe"
}

build_and_run island_pipe_tests
build_and_run notification_center_tests
