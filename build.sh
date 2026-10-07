#!/bin/bash
# Cross-compile the mod with llvm-mingw (https://github.com/mstorsjo/llvm-mingw) or any x86_64 mingw-w64
# compiler:   CC=/path/to/x86_64-w64-mingw32-clang ./build.sh [dinput8|dbghelp]
# The argument is the system DLL whose name the mod takes in the game folder (default dinput8).
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-clang}
NAME=${1:-dinput8}
mkdir -p out
python3 tools/genproxy.py "$NAME" >/dev/null
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-cast-function-type-mismatch -Wno-misleading-indentation \
    -shared -static -o "out/$NAME.dll" src/*.c src/proxy_gen.S "out/$NAME.def" -Wl,--kill-at -lkernel32 -luser32
ls -la "out/$NAME.dll"
