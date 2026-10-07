#!/usr/bin/env python3
"""Build bundle_src/Factory_revenge.lub: the game's Factory.lub with the Revenge Value code added.

  BBS_LUA51        a Lua 5.1 interpreter built for the game's bytecode format: float numbers, 4-byte size_t
                   (stock Lua 5.1 with LUA_NUMBER float in luaconf.h and the string length written / read as a
                   32-bit int in ldump.c / lundump.c; see docs/NOTES.md), and stripping when LUA_STRIP is set
  BBS_FACTORY_LUB  the original Factory.lub from arc/system/CommonLua.arc (638 bytes)

The source is bundle_src/revenge/revenge.lua with bosses.lua put in at __BOSS_CONFIG__.  The original Factory.lub
is not recompiled: its main function is spliced in, byte for byte, as the first nested function and called first.
"""
import os, struct, subprocess, sys, tempfile
R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LUA = os.environ.get('BBS_LUA51', '/home/claude/bbs/tools/lua51/lua')
ORIG = os.environ['BBS_FACTORY_LUB']
HDR = bytes.fromhex('1b4c75615100010404040400')


def proto_end(d, p):
    """offset just past the function prototype starting at p (int 4, size_t 4, float numbers)"""
    def u32():
        nonlocal p
        v = struct.unpack_from('<I', d, p)[0]; p += 4
        return v
    def string():
        nonlocal p
        n = u32(); p += n
    def skip(k):
        nonlocal p
        n = u32(); p += k * n
    string(); p += 8 + 4                      # source, linedefined, lastlinedefined, nups/nparams/vararg/maxstack
    skip(4)                                   # code
    for _ in range(u32()):                    # constants
        t = d[p]; p += 1
        if t == 1: p += 1
        elif t == 3: p += 4
        elif t == 4: string()
        elif t != 0: raise SystemExit('bad constant type %d' % t)
    for _ in range(u32()): p = proto_end(d, p)
    skip(4)                                   # line info
    for _ in range(u32()): string(); p += 8   # locals
    for _ in range(u32()): string()           # upvalue names
    return p


def first_child(d, p):
    """(start, end) of the first nested prototype of the prototype at p"""
    def u32():
        nonlocal p
        v = struct.unpack_from('<I', d, p)[0]; p += 4
        return v
    n = u32(); p += n + 12
    n = u32(); p += 4 * n
    for _ in range(u32()):
        t = d[p]; p += 1
        if t == 1: p += 1
        elif t == 3: p += 4
        elif t == 4: n = u32(); p += n
    n = u32(); assert n >= 1
    return p, proto_end(d, p)


def main():
    src = open(os.path.join(R, 'bundle_src', 'revenge', 'revenge.lua')).read()
    cfg = open(os.path.join(R, 'bundle_src', 'revenge', 'bosses.lua')).read()
    assert '__FACTORY_ORIGINAL__()' in src and '__BOSS_CONFIG__' in src
    src = src.replace('__FACTORY_ORIGINAL__()', '(function() end)()').replace('__BOSS_CONFIG__', cfg)
    orig = open(ORIG, 'rb').read()
    assert orig[:12] == HDR and proto_end(orig, 12) == len(orig), 'unexpected Factory.lub'
    with tempfile.TemporaryDirectory() as t:
        a, b = os.path.join(t, 'in.lua'), os.path.join(t, 'out.lub')
        open(a, 'w').write(src)
        subprocess.run([LUA, '-e', 'local f=assert(loadfile(%r)) local o=assert(io.open(%r,"wb")) o:write(string.dump(f)) o:close()' % (a, b)],
                       check=True, env=dict(os.environ, LUA_STRIP='1'))
        d = open(b, 'rb').read()
    assert d[:12] == HDR, 'the Lua interpreter does not write the game format'
    s, e = first_child(d, 12)
    out = d[:s] + orig[12:] + d[e:]
    assert proto_end(out, 12) == len(out)
    dst = os.path.join(R, 'bundle_src', 'Factory_revenge.lub')
    old = open(dst, 'rb').read() if os.path.isfile(dst) else b''
    if '--check' in sys.argv:
        print('identical to the existing file' if out == old else 'DIFFERENT from the existing file (%d vs %d bytes)' % (len(out), len(old)))
        return
    open(dst, 'wb').write(out)
    print('Factory_revenge.lub: %d bytes%s' % (len(out), '' if out != old else ' (unchanged)'))


main()
