#!/bin/bash
# Runs the loop test one boss script per process (the stand-in engine's globals
# are shared, and scripts contaminate each other when loaded into one state).
#   BBS_LUA51=<patched lua 5.1> test/revenge/loop.sh
cd "$(dirname "$0")/../.."
LUA="${BBS_LUA51:?set BBS_LUA51 to the patched Lua 5.1 interpreter}"
bad=0
for s in $("$LUA" test/revenge/loop.lua list); do
  out=$("$LUA" test/revenge/loop.lua "$s" 2>&1); rc=$?
  echo "$out" | grep -v "^LOOP TEST"
  [ $rc -ne 0 ] && bad=1
done
if [ $bad -eq 0 ]; then echo "LOOP TEST OK"; else echo "LOOP TEST FAILED"; exit 1; fi
