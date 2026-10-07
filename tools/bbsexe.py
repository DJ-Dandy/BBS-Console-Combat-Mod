"""Shared helpers for the generator scripts: read the game executable, find code references, disassemble.

The scripts need your own copy of the game executable (it is not part of this repository):
    export KH1_EXE="/path/to/KINGDOM HEARTS FINAL MIX.exe"
They also need `objdump` (binutils) and numpy.
"""
import bisect, hashlib, os, struct, subprocess, sys

import numpy as np

EXPECTED_SHA256 = '375a811243f1f95f786f00318e2e1210dd5881b25bbf0912a0ad8f81650c976c'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def exe_path():
    p = os.environ.get('BBS_EXE','/home/claude/bbs/bbs.exe')
    if not p or not os.path.isfile(p):
        sys.exit('Set BBS_EXE to the path of "KINGDOM HEARTS Birth by Sleep FINAL MIX.exe" (Steam build, unmodified).')
    return p


class PE:
    def __init__(s, path):
        s.path = path
        s.d = bytearray(open(path, 'rb').read())
        d = s.d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        s.base = struct.unpack_from('<Q', d, opt + 24)[0]
        s.secs = []
        for i in range(nsec):
            o = opt + optsz + i * 40
            name = d[o:o + 8].rstrip(b'\0').decode()
            vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
            s.secs.append((name, va, vs, ro, rs))
        # exception directory (.pdata): one entry per function, used to find function starts
        ex_rva, ex_size = struct.unpack_from('<II', d, opt + 112 + 3 * 8)
        s.func_starts = sorted({struct.unpack_from('<I', d, s.off(ex_rva) + i)[0] for i in range(0, ex_size, 12)})

    def off(s, rva):
        for n, va, vs, ro, rs in s.secs:
            if va <= rva < va + rs:
                return ro + rva - va
        return None

    def func_of(s, rva):
        i = bisect.bisect_right(s.func_starts, rva) - 1
        return s.func_starts[i] if i >= 0 else None


_pe = None


def pe():
    global _pe
    if _pe is None:
        _pe = PE(exe_path())
        h = hashlib.sha256(bytes(_pe.d)).hexdigest()
        if h != EXPECTED_SHA256:
            print('warning: executable hash %s differs from the build this mod was written for' % h, file=sys.stderr)
    return _pe


def dis(start, stop):
    """objdump text for the RVA range [start, stop)"""
    p = pe()
    r = subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn',
                        '--start-address=0x%x' % (p.base + start), '--stop-address=0x%x' % (p.base + stop), p.path],
                       capture_output=True, text=True).stdout
    return '\n'.join(l for l in r.split('\n') if l.startswith('   1'))


def xrefs(lo, hi):
    """(rva of the disp32, target rva, bytes between the disp32 and the end of its instruction)
    for every RIP-relative reference in code whose target lies in [lo, hi)"""
    p = pe()
    out = []
    for name, va, vs, ro, rs in p.secs:
        if name not in ('.text', '.nep'):
            continue
        b = np.frombuffer(bytes(p.d[ro:ro + rs]), dtype=np.uint8)
        n = len(b) - 4
        disp = (b[0:n].astype(np.int64) | (b[1:n + 1].astype(np.int64) << 8) |
                (b[2:n + 2].astype(np.int64) << 16) | (b[3:n + 3].astype(np.int64) << 24))
        disp = np.where(disp >= 2 ** 31, disp - 2 ** 32, disp)
        pos = np.arange(n, dtype=np.int64) + va
        for extra in (0, 1, 4, 2):
            t = pos + 4 + extra + disp
            for i in np.nonzero((t >= lo) & (t < hi))[0]:
                out.append((int(pos[i]), int(t[i]), extra))
    return sorted(out)
