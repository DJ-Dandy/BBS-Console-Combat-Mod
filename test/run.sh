#!/bin/bash
# Offline check (Linux, x86-64): maps your copy of the game executable into memory, applies every
# patch, and runs some of the patched game code natively.  Needs gcc.
#   BBS_EXE="/path/to/KINGDOM HEARTS Birth by Sleep FINAL MIX.exe" test/run.sh
set -e
cd "$(dirname "$0")/.."
: "${BBS_EXE:?set BBS_EXE to the path of KINGDOM HEARTS Birth by Sleep FINAL MIX.exe}"
mkdir -p out
gcc -O1 -g -w -Itest/inc -o out/run test/run.c src/core.c src/mp.c src/bundle.c src/menu.c src/tex.c src/style.c src/speed.c src/status.c src/shortcut.c src/sccamp.c src/guard.c -lm
./out/run "$BBS_EXE"
