#!/bin/sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out/tests"
COMPILER="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
mkdir -p "$OUT"
{
  echo "#include \"island_pipe_stubs.h\""
  awk '/^namespace island_pipe$/,/^bool StartThreads\(\) \{/' "$HERE/dynamic-island-openmeteo.wh.cpp" | sed '$d'
} > "$OUT/island_pipe_under_test.h"
"$COMPILER" -std=c++23 -O2 -Wall -Wextra -Werror -static -DUNICODE -D_UNICODE -target x86_64-w64-mingw32 \
  -I "$OUT" -I "$HERE/tests" "$HERE/tests/island_pipe_tests.cpp" -o "$OUT/island_pipe_tests.exe" -ladvapi32
"$OUT/island_pipe_tests.exe"
