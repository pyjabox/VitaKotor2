"""plt_oracle.py -- which of plt_scan's call sites plt_direct.c may patch.

usage: plt_oracle.py libkotor2.so sites.txt loader/plt_exclude.h
  sites.txt: written by tools/test_plt_scan.c (the sites plt_direct.c would
  patch: blx/b.w through the PLT to functions defined in libkotor2).
  needs: pip install capstone pyelftools

plt_scan sweeps .text linearly, as objdump does, so data that happens to look
like a branch to a PLT entry would be taken for a call. This disassembles the
other way: from every function entry (dynamic symbols, init/fini arrays,
.ARM.exidx, relocated function pointers, bl targets), following control flow,
twice -- the second pass stops at the literals and jump tables the first one
found. A site is kept only if that reaches it as an instruction and it touches
no literal or table. The rest (C++ landing pads, reached only by the unwinder,
and a few overlaps) go to the exclusion list, with the count and hash
plt_direct.c checks its own scan against before it patches anything."""
import sys, struct, re, bisect
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile
so, sites_path, out_path = sys.argv[1:4]
import hashlib
data = open(so, 'rb').read()
elf = ELFFile(open(so, 'rb'))
sec = {s.name: s for s in elf.iter_sections()}
text = sec['.text']; TLO = text['sh_addr']; THI = TLO + text['sh_size']
plt = sec['.plt']; PLT0 = plt['sh_addr'] + 20
dynsym = sec['.dynsym']
syms = list(dynsym.iter_symbols())
relplt = list(sec['.rel.plt'].iter_relocations())
NPLT = len(relplt)
arm_lo = min(s['st_value'] for s in syms if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF'
             and not s['st_value'] & 1 and TLO <= s['st_value'] < THI)
print('text %x-%x, thumb part to %x' % (TLO, THI, arm_lo))
NORET = re.compile(r'stack_chk_fail|^abort$|^exit$|^_exit$|__assert|cxa_throw|cxa_rethrow|throw_|terminate|Unwind_Resume|bad_cast|bad_typeid|pure_virtual|deleted_virtual|longjmp|abort_message|__cxa_call_unexpected|pthread_exit')
plt_name = {}
noret_plt = set()
for i, r in enumerate(relplt):
    nm = syms[r['r_info_sym']].name
    plt_name[PLT0 + 12 * i] = nm
    if NORET.search(nm): noret_plt.add(PLT0 + 12 * i)
noret_fn = set()
entries = set()
for s in syms:
    if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF' and s['st_value'] & 1:
        a = s['st_value'] & ~1
        if TLO <= a < arm_lo:
            entries.add(a)
            if NORET.search(s.name): noret_fn.add(a)
for nm in ('.init_array', '.fini_array'):
    if nm in sec:
        b = sec[nm].data()
        for (v,) in struct.iter_unpack('<I', b):
            if v & 1 and TLO <= (v & ~1) < arm_lo: entries.add(v & ~1)
ex = sec['.ARM.exidx']; b = ex.data(); base = ex['sh_addr']
for i in range(0, len(b), 8):
    w = struct.unpack_from('<I', b, i)[0]
    off = (w & 0x7fffffff) - ((w & 0x40000000) << 1)
    fn = (base + i + off) & 0xffffffff
    if TLO <= fn < arm_lo: entries.add(fn)
nrel = 0
for r in sec['.rel.dyn'].iter_relocations():
    if r['r_info_type'] == 23:  # R_ARM_RELATIVE
        o = r['r_offset']
        sh = next((s for s in elf.iter_sections() if s['sh_addr'] <= o < s['sh_addr'] + s['sh_size'] and s['sh_type'] != 'SHT_NOBITS'), None)
        if not sh: continue
        v = struct.unpack_from('<I', data, sh['sh_offset'] + o - sh['sh_addr'])[0]
        if v & 1 and TLO <= (v & ~1) < arm_lo: entries.add(v & ~1); nrel += 1
print('entries', len(entries), '(relative fn pointers %d)' % nrel)
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
def u16(a): return data[a] | data[a + 1] << 8
def isize(a):
    h = u16(a); return 4 if (h & 0xf800) >= 0xe800 else 2
decoded = {}
def dec(a):
    r = decoded.get(a)
    if r is None:
        l = list(md.disasm(data[a:a + 4], a, 1))
        r = (l[0].mnemonic, l[0].op_str, l[0].size) if l else None
        decoded[a] = r
    return r
TGT = re.compile(r'#0x([0-9a-f]+)')
def run(datarange_starts, datarange):
    code = set(); lits = []; work = list(entries); seen_entry = set(work)
    def is_data(a):
        i = bisect.bisect_right(datarange_starts, a) - 1
        return i >= 0 and datarange[i][0] <= a < datarange[i][1]
    while work:
        a = work.pop()
        it_left = 0; recent = []
        while TLO <= a < arm_lo and a not in code:
            if is_data(a): break
            d = dec(a)
            if d is None: break
            mn, op, sz = d
            code.add(a)
            cond = it_left > 0
            if it_left: it_left -= 1
            nxt = a + sz
            base_mn = mn.split('.')[0]
            if base_mn == 'it' or mn.startswith('it'):
                if mn.startswith('it') and mn not in ('it',) or mn == 'it':
                    it_left = len(mn) - 1  # it, itt, ite, ittt...
                a = nxt; continue
            # pc-relative literal
            if '[pc' in op and (base_mn.startswith('ldr') or base_mn.startswith('vldr')):
                m = re.search(r'\[pc, #(-?0x[0-9a-f]+|-?\d+)\]', op)
                off = int(m.group(1), 0) if m else 0
                la = ((a + 4) & ~3) + off
                w = 8 if base_mn.startswith('ldrd') or (base_mn == 'vldr' and op.startswith('d')) else 4
                lits.append((la, la + w))
            if base_mn in ('tbb', 'tbh'):
                k = None
                for pm, po in reversed(recent[-6:]):
                    mm = re.match(r'(\w+), #(0x[0-9a-f]+|\d+)$', po)
                    if pm.startswith('cmp') and mm: k = int(mm.group(2), 0); break
                tb = a + 4; ent = 1 if base_mn == 'tbb' else 2
                n = (k + 1) if k is not None else None
                i = 0; mint = None
                while True:
                    if n is not None and i >= n: break
                    if n is None and mint is not None and i * ent >= mint: break
                    e = data[tb + i * ent] if ent == 1 else u16(tb + i * ent)
                    t = tb + 2 * e if False else a + 4 + 2 * e
                    mint = (t - tb) if mint is None else min(mint, t - tb)
                    if t not in seen_entry: work.append(t); seen_entry.add(t)
                    i += 1
                    if i > 4096: break
                lits.append((tb, tb + ((i * ent + 1) & ~1)))
                break
            regs = op
            if base_mn in ('bx',) and not cond: break
            if base_mn in ('pop', 'ldm', 'ldmia') and 'pc' in regs and not cond: break
            if base_mn.startswith('ldr') and regs.startswith('pc') and not cond: break
            if base_mn == 'mov' and regs.startswith('pc') and not cond: break
            if base_mn in ('udf', 'bkpt'): break
            m = TGT.match(op)
            if base_mn in ('b', 'beq', 'bne', 'bcs', 'bcc', 'bmi', 'bpl', 'bvs', 'bvc', 'bhi', 'bls', 'bge', 'blt', 'bgt', 'ble', 'bhs', 'blo', 'cbz', 'cbnz') or (base_mn.startswith('b') and base_mn[1:] in ('eq','ne','cs','cc','mi','pl','vs','vc','hi','ls','ge','lt','gt','le','hs','lo')):
                if base_mn in ('cbz', 'cbnz'): m = re.search(r'#0x([0-9a-f]+)', op)
                if m:
                    t = int(m.group(1), 16)
                    if TLO <= t < arm_lo and t not in seen_entry: work.append(t); seen_entry.add(t)
                if base_mn == 'b' and not cond: break
                a = nxt; recent.append((mn, op)); continue
            if base_mn in ('bl', 'blx') and m:
                t = int(m.group(1), 16)
                if base_mn == 'bl' and TLO <= t < arm_lo and t not in seen_entry: work.append(t); seen_entry.add(t)
                if (t in noret_plt or t in noret_fn) and not cond: break
            recent.append((mn, op))
            a = nxt
    return code, lits
code, lits = run([], [])
lits.sort(); ls = [l[0] for l in lits]
print('pass 1: %d instructions, %d literal/table ranges' % (len(code), len(lits)))
code2, lits2 = run(ls, lits)
print('pass 2: %d instructions' % len(code2))
allr = sorted(set(lits) | set(lits2)); als = [l[0] for l in allr]
def in_data(a, n=4):
    i = bisect.bisect_right(als, a + n - 1) - 1
    while i >= 0 and allr[i][0] > a - 4100:
        if allr[i][0] < a + n and allr[i][1] > a: return True
        i -= 1
    return False
c = [tuple(l.split()) for l in open(sites_path)]
h = 2166136261
for a, idx, kind in c:
    for w in (int(a, 16), int(idx), int(kind)):
        for b in range(4):
            h ^= (w >> (8 * b)) & 0xff; h = (h * 16777619) & 0xffffffff
excl = []
for a, idx, kind in c:
    a = int(a, 16)
    if in_data(a) or a not in code2: excl.append(a)
excl.sort()
print('%d sites, hash 0x%08x: %d excluded, %d to patch' % (len(c), h, len(excl), len(c) - len(excl)))
sha = hashlib.sha256(data).hexdigest()
with open(out_path, 'w') as f:
    f.write('/* plt_exclude.h -- generated by tools/plt_oracle.py; do not edit.\n')
    f.write(' * libkotor2.so sha256 %s\n' % sha)
    f.write(' * plt_scan finds %d patchable sites (hash below); these %d are not\n' % (len(c), len(excl)))
    f.write(' * reached as code from a function entry, or touch a literal or table. */\n')
    f.write('#define PLT_EXPECT_SITES %du\n#define PLT_EXPECT_HASH 0x%08xu\n' % (len(c), h))
    f.write('static const uint32_t plt_exclude[%d] = {\n' % len(excl))
    for i in range(0, len(excl), 8):
        f.write('  ' + ', '.join('0x%06x' % x for x in excl[i:i + 8]) + ',\n')
    f.write('};\n')
