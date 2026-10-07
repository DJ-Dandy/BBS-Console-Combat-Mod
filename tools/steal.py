#!/usr/bin/env python3
"""steal.py NAME RVA [minlen]  ->  C 'Steal' initializer for a hook site.

A hook overwrites the first bytes at RVA with a jump.  This works out which whole instructions that
covers and where their RIP-relative displacements are, so the patcher can run them from a trampoline.
"""
import re, struct, subprocess, sys
from bbsexe import pe


def insns(rva, n=48):
    p = pe()
    out = subprocess.run(['objdump', '-d', '-M', 'intel', '--start-address=0x%x' % (p.base + rva),
                          '--stop-address=0x%x' % (p.base + rva + n), p.path], capture_output=True, text=True).stdout
    res = []
    for l in out.split('\n'):
        m = re.match(r'\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*(?:\t(.*))?$', l)
        if not m:
            continue
        a = int(m.group(1), 16) - p.base
        b = bytes.fromhex(m.group(2).replace(' ', ''))
        t = m.group(3) or ''
        if res and not t:          # continuation line of bytes
            res[-1] = (res[-1][0], res[-1][1] + b, res[-1][2])
            continue
        res.append((a, b, t))
    return res


def steal(name, rva, minlen=5):
    """instruction-aligned bytes that will be executed from a trampoline (RIP fixups computed)"""
    base = pe().base
    tot = 0
    chosen = []
    for a, b, t in insns(rva):
        if tot >= minlen:
            break
        chosen.append((a, b, t))
        tot += len(b)
    data = b''.join(b for a, b, t in chosen)
    fix = []
    fixend = []
    for a, b, t in chosen:
        off = a - rva
        tgt = None
        m = re.search(r'# 0x([0-9a-f]+)', t)
        if m and 'rip' in t:
            tgt = int(m.group(1), 16) - base
        else:
            m2 = re.match(r'(call|jmp|j[a-z]+)\s+0x([0-9a-f]+)', t)
            if m2:
                tgt = int(m2.group(2), 16) - base
                if len(b) == 2:
                    raise SystemExit('short jump in stolen bytes at %x: %s' % (a, t))
        if tgt is not None:
            end = a + len(b)
            found = None
            for k in range(len(b) - 4, -1, -1):
                if end + struct.unpack_from('<i', b, k)[0] == tgt:
                    found = k
                    break
            if found is None:
                raise SystemExit('cannot locate disp at %x: %s' % (a, t))
            fix.append(off + found)
            fixend.append(off + len(b))
        if re.match(r'(ret|int3)', t):
            raise SystemExit('ret/int3 inside stolen bytes at %x' % a)
    assert len(data) <= 24 and len(fix) <= 4, (len(data), fix)
    c = 'static const Steal %s = { 0x%x, %d, %d, {%s}, {%s}, {%s} };' % (
        name, rva, len(data), len(fix), ','.join(map(str, fix)) or '0', ','.join(map(str, fixend)) or '0',
        ','.join('0x%02x' % x for x in data))
    return c + ' /* ' + ' ; '.join(t.split('#')[0].strip() for a, b, t in chosen) + ' */'


def raw(name, rva, n):
    """bytes that are never executed (the handler always redirects): no fixups, short jumps allowed"""
    tot = 0
    chosen = []
    for a, b, t in insns(rva, n + 16):
        if tot >= n:
            break
        chosen.append((a, b, t))
        tot += len(b)
    assert tot == n, ('not instruction aligned', hex(rva), n, tot)
    data = b''.join(b for a, b, t in chosen)
    c = 'static const Steal %s = { 0x%x, %d, 0, {0}, {0}, {%s} };' % (name, rva, len(data), ','.join('0x%02x' % x for x in data))
    return c + ' /* NOEXEC: ' + ' ; '.join(t.split('#')[0].strip() for a, b, t in chosen) + ' */'


if __name__ == '__main__':
    print(steal(sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 5))
