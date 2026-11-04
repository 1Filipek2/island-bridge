#!/bin/sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out/tests"
COMPILER="${MOD_TEST_COMPILER:-windhawk}"
mkdir -p "$OUT"
{
  echo "#include \"island_pipe_stubs.h\""
  awk '/^namespace island_pipe$/,/^bool StartThreads\(\) \{/' "$HERE/dynamic-island-openmeteo.wh.cpp" | sed '$d'
} > "$OUT/island_pipe_under_test.h"
if [ "$COMPILER" = "msvc" ]; then
  MSYS2_ARG_CONV_EXCL='*' cl.exe /nologo /std:c++latest /utf-8 /EHsc /W4 /WX /O2 /DUNICODE /D_UNICODE \
    /I "$(cygpath -w "$OUT")" /I "$(cygpath -w "$HERE/tests")" "$(cygpath -w "$HERE/tests/island_pipe_tests.cpp")" \
    /Fo"$(cygpath -w "$OUT")\\" /Fe"$(cygpath -w "$OUT/island_pipe_tests.exe")" /link advapi32.lib
else
  "/c/Program Files/Windhawk/Compiler/bin/clang++.exe" -std=c++23 -O2 -Wall -Wextra -Werror -static -DUNICODE -D_UNICODE -target x86_64-w64-mingw32 \
    -I "$OUT" -I "$HERE/tests" "$HERE/tests/island_pipe_tests.cpp" -o "$OUT/island_pipe_tests.exe" -ladvapi32
fi
"$OUT/island_pipe_tests.exe"
