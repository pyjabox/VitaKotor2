#!/usr/bin/env python3
"""gen_aslw.py <libkotor2.so> <out.c> -- recorders for ASLgl on the GL worker.

Every ASLgl function with a relocation slot in libkotor2 (2.0.4: 258) gets:
  - aslw_gen_<n>: the default recorder, put in the function's slots. On the
    GL worker (or with the worker off) it calls the function through; on the
    game thread it records the call into the GL worker's stream;
  - a case in aslw_replay_gen(), where the worker calls the function;
  - an entry in aslw_fns[] (mangled name, recorder, original, custom).
A function named in loader/asl_worker.c as aslw_custom_<n> (weak reference
here) is installed instead of the default: draws, answers from state copies,
state tracking. It can still record through aslw_gen_<n>.

The engine is softfp: floats arrive in core registers, doubles in register
pairs. Arguments are therefore integer carriers (uint32_t, uint64_t), which
the hardfp loader passes the same way.

Calls are recorded three ways:
  - ASYNC: no return value, no pointer argument; the arguments go in the
    stream and the game thread carries on;
  - COPY: no return value, every pointer argument read-only with a size rule
    below (SIZE): the data is copied into the stream (over the stream's inline
    limit, the call runs as SYNC instead);
  - SYNC: everything else -- a return value, an output pointer, an input
    without a size rule. The game thread waits while the worker runs the call
    with the caller's own pointers.
Return types are not in mangled names: VALUE lists the functions that return
one (GL naming; every other ASLgl function was checked against its callers,
none of which reads r0 after the call)."""

import os
import re
import struct
import subprocess
import sys

VALUE = re.compile(r'^(glGetError|glGetString|glIs\w+|glCheck\w+|glCreate\w+|glGenLists|glGetAttribLocation|'
                   r'glGetUniformLocation|glMapBuffer\w*|glUnmapBuffer\w*|glTest\w+|glRenderMode|'
                   r'glGetUniformBufferSizeEXT|glGetUniformOffsetEXT|glGetHandleARB)$')

# Size in bytes of a read-only pointer argument, as a C expression over p0..pN.
# Helpers aslw_n_*() (asl_worker.c) give a pname's value count.
N = {'1': 1, '2': 2, '3': 3, '4': 4}
SIZE = {}
for m in ('glLoadMatrixf', 'glMultMatrixf', 'glLoadTransposeMatrixf', 'glMultTransposeMatrixf'):
    SIZE[m] = {0: '64'}
for m in ('glLoadMatrixd', 'glMultMatrixd'):
    SIZE[m] = {0: '128'}
SIZE['glClipPlane'] = {1: '32'}
for m in ('glProgramEnvParameter4fvARB', 'glProgramLocalParameter4fvARB'):
    SIZE[m] = {2: '16'}
for m in ('glProgramEnvParameters4fvEXT', 'glProgramLocalParameters4fvEXT'):
    SIZE[m] = {3: 'p2 * 16'}
for k in '1234':
    for t in 'fi':
        SIZE[f'glUniform{k}{t}v'] = {2: f'p1 * {k} * 4'}
        SIZE[f'glUniform{k}{t}vARB'] = {2: f'p1 * {k} * 4'}
for k in '234':
    SIZE[f'glUniformMatrix{k}fv'] = {3: f'p1 * {k} * {k} * 4'}
    SIZE[f'glUniformMatrix{k}fvARB'] = {3: f'p1 * {k} * {k} * 4'}
for name, n in (('glColor3fv', 12), ('glColor4fv', 16), ('glColor3ubv', 3), ('glColor4ubv', 4), ('glNormal3fv', 12),
                ('glVertex2fv', 8), ('glVertex3fv', 12), ('glVertex4fv', 16), ('glTexCoord2fv', 8),
                ('glTexCoord3fv', 12), ('glTexCoord4fv', 16), ('glSecondaryColor3fv', 12),
                ('glSecondaryColor3ubv', 3), ('glRasterPos2fv', 8), ('glRasterPos3fv', 12)):
    SIZE[name] = {0: str(n)}
for k in '1234':
    SIZE[f'glMultiTexCoord{k}fvARB'] = {1: f'{k} * 4'}
    SIZE[f'glMultiTexCoord{k}fv'] = {1: f'{k} * 4'}
    SIZE[f'glVertexAttrib{k}fv'] = {1: f'{k} * 4'}
    SIZE[f'glVertexAttrib{k}fvARB'] = {1: f'{k} * 4'}
for m in ('glLightfv', 'glLightiv'):
    SIZE[m] = {2: 'aslw_n_light(p1) * 4'}
for m in ('glMaterialfv', 'glMaterialiv'):
    SIZE[m] = {2: 'aslw_n_material(p1) * 4'}
for m in ('glLightModelfv', 'glLightModeliv'):
    SIZE[m] = {1: 'aslw_n_lightmodel(p0) * 4'}
for m in ('glFogfv', 'glFogiv'):
    SIZE[m] = {1: 'aslw_n_fog(p0) * 4'}
for m in ('glTexEnvfv', 'glTexEnviv'):
    SIZE[m] = {2: 'aslw_n_texenv(p1) * 4'}
for m in ('glTexParameterfv', 'glTexParameteriv'):
    SIZE[m] = {2: 'aslw_n_texparam(p1) * 4'}
for m in ('glTexGenfv', 'glTexGeniv'):
    SIZE[m] = {2: 'aslw_n_texgen(p1) * 4'}
for m in ('glPointParameterfv', 'glPointParameterfvARB', 'glPointParameteriv'):
    SIZE[m] = {1: 'aslw_n_pointparam(p0) * 4'}
for m in ('glDeleteTextures', 'glDeleteBuffers', 'glDeleteBuffersARB', 'glDeleteProgramsARB', 'glDeleteQueriesARB',
          'glDeleteQueries', 'glDeleteFramebuffers', 'glDeleteFramebuffersEXT', 'glDeleteRenderbuffers',
          'glDeleteRenderbuffersEXT', 'glDeleteFencesAPPLE', 'glDeleteVertexArraysOES', 'glDrawBuffers',
          'glDrawBuffersARB'):
    SIZE[m] = {1: 'p0 * 4'}
for m in ('glBufferData', 'glBufferDataARB'):
    SIZE[m] = {2: 'p1'}
for m in ('glBufferSubData', 'glBufferSubDataARB'):
    SIZE[m] = {3: 'p2'}
SIZE['glTexImage2D'] = {8: 'aslw_pixels_size(p3, p4, p6, p7)'}
SIZE['_glTexImage2D'] = SIZE['glTexImage2D']
SIZE['glTexSubImage2D'] = {8: 'aslw_pixels_size(p4, p5, p6, p7)'}
SIZE['glCompressedTexImage2D'] = {7: 'p6'}
SIZE['glCompressedTexImage2DARB'] = {7: 'p6'}
SIZE['glCompressedTexSubImage2D'] = {8: 'p7'}
SIZE['glCompressedTexSubImage2DARB'] = {8: 'p7'}
SIZE['glProgramStringARB'] = {3: 'p2'}
SIZE['glCallLists'] = {2: 'p0 * aslw_type_size(p1)'}
SIZE['glPolygonStipple'] = {0: '128'}

# Recorded by hand (asl_worker.c): their default recorder must not be the
# one installed, so these must have an aslw_custom_<n>; the generator checks
# that a matching definition exists in asl_worker.c.
CUSTOM = {
    'glDrawElements', 'glDrawArrays', 'ASLglDrawQuadArrays',
    'ASLglDrawQuadArraysStartIndex', 'glVertexPointer', 'glColorPointer', 'glTexCoordPointer', 'glNormalPointer',
    'glSecondaryColorPointer', 'glVertexAttribPointer', 'glVertexAttribPointerARB',
}

CARRIER = {
    'unsigned int': 'uint32_t', 'int': 'uint32_t', 'float': 'uint32_t', 'unsigned char': 'uint32_t',
    'char': 'uint32_t', 'signed char': 'uint32_t', 'short': 'uint32_t', 'unsigned short': 'uint32_t',
    'long': 'uint32_t', 'unsigned long': 'uint32_t', 'bool': 'uint32_t',
    'double': 'uint64_t', 'long long': 'uint64_t', 'unsigned long long': 'uint64_t',
}


def elf_reloc_symbols(path):
    """Names of the dynamic symbols that relocations (REL, PLT) refer to, with type."""
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 32)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 46)
    secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    def sec_by_type(t): return [s for s in secs if s[1] == t]
    dynsym = sec_by_type(11)[0]
    dynstr = secs[dynsym[6]]
    def name_at(off):
        e = d.index(b'\0', dynstr[4] + off)
        return d[dynstr[4] + off:e].decode()
    syms = []
    for i in range(dynsym[5] // 16):
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', d, dynsym[4] + i * 16)
        syms.append((name_at(st_name), st_info & 0xf))
    out = {}
    for s in sec_by_type(9):          # SHT_REL
        for i in range(s[5] // 8):
            r_offset, r_info = struct.unpack_from('<II', d, s[4] + i * 8)
            idx = r_info >> 8
            if idx and (r_info & 0xff) in (2, 21, 22):   # ABS32, GLOB_DAT, JUMP_SLOT
                out[syms[idx][0]] = syms[idx][1]
    return out


def parse(dem):
    name = dem[len('ASLgl::'):dem.index('(')]
    params = dem[dem.index('(') + 1:dem.rindex(')')].strip()
    ps = [] if params in ('', 'void') else [p.strip() for p in params.split(',')]
    return name, ps


def main():
    so, out = sys.argv[1], sys.argv[2]
    rel = elf_reloc_symbols(so)
    mangled = sorted(m for m, t in rel.items() if m.startswith('_ZN5ASLgl') and t == 2)
    dem = subprocess.run(['c++filt'], input='\n'.join(mangled), capture_output=True, text=True).stdout.split('\n')
    custom_src = open(os.path.join(os.path.dirname(out), 'asl_worker.c')).read() \
        if os.path.exists(os.path.join(os.path.dirname(out), 'asl_worker.c')) else ''
    fns = []
    for m, dm in zip(mangled, dem):
        name, ps = parse(dm)
        if '::' in name:                # members of ASLgl's own classes (static constructors): not GL calls
            continue
        args, kinds = [], []
        for i, t in enumerate(ps):
            if t in CARRIER:
                args.append(CARRIER[t])
                kinds.append('w64' if CARRIER[t] == 'uint64_t' else 'w')
            elif t.endswith('*'):
                args.append('uintptr_t')
                const = ' const' in t.rsplit('*', 1)[0] or t.startswith('const ')
                kinds.append('in' if const and t.count('*') == 1 else 'out')
            else:
                raise SystemExit(f'unknown type {t!r} in {dm}')
        ret = bool(VALUE.match(name))
        ptrs = [i for i, k in enumerate(kinds) if k in ('in', 'out')]
        rule = SIZE.get(name, {})
        if ret or any(kinds[i] == 'out' for i in ptrs) or any(i not in rule for i in ptrs):
            cls = 'SYNC'
        elif ptrs:
            cls = 'COPY'
        else:
            cls = 'ASYNC'
        if name in CUSTOM and f'aslw_custom_{name}(' not in custom_src:
            print(f'warning: {name} is CUSTOM but asl_worker.c defines no aslw_custom_{name}', file=sys.stderr)
        fns.append(dict(m=m, dem=dm, name=name, args=args, kinds=kinds, ret=ret, cls=cls, rule=rule))

    L = []
    w = L.append
    w('/* aslw_gen.c -- generated by tools/gen_aslw.py from libkotor2 2.0.4: recorders and replay for')
    w(' * every ASLgl function with a relocation slot. Do not edit; see asl_worker.c. */')
    w('')
    w('#include "asl_worker_internal.h"')
    w('')
    w('#if ASLW')
    w('')
    for i, f in enumerate(fns):
        c = f['name']
        params = ', '.join(f'{t} p{j}' for j, t in enumerate(f['args'])) or 'void'
        types = ', '.join(f['args']) or 'void'
        rtype = 'uint32_t' if f['ret'] else 'void'
        w(f'/* {f["dem"]} -- {f["cls"]} */')
        w(f'static {rtype} (*o_{c})({types});')
        w(f'extern {rtype} aslw_custom_{c}({params}) __attribute__((weak));')
        w(f'{rtype} aslw_gen_{c}({params}) {{')
        callargs = ', '.join(f'p{j}' for j in range(len(f['args'])))
        if f['ret']:
            w(f'  if (ASLW_DIRECT()) {{ uint32_t r; ASLW_CALL(r = o_{c}({callargs})); return r; }}')
            w('  {')
        else:
            w(f'  if (ASLW_DIRECT()) ASLW_CALL(o_{c}({callargs})); else {{')
        nw = sum(2 if k == 'w64' else 1 for k in f['kinds'])
        op = f'ASLW_OP_BASE + {i}'
        sync = f['cls'] == 'SYNC'
        if f['cls'] == 'COPY':
            # data sizes, total, inline or in place
            sizes = [(j, f['rule'][j]) for j, k in enumerate(f['kinds']) if k == 'in']
            for j, expr in sizes:
                w(f'    uint32_t n{j} = p{j} ? (uint32_t)({expr}) : 0;')
            tot = ' + '.join(f'((n{j} + 3) & ~3u)' for j, _ in sizes)
            unknown = ' || '.join(f'n{j} == ASLW_SIZE_UNKNOWN' for j, _ in sizes)
            w(f'    int unknown = {unknown};')
            w(f'    uint32_t bytes = unknown ? 0 : {tot};')
            w(f'    int in_place = unknown || bytes > ASLW_INLINE_MAX;')
            w(f'    uint32_t *a = glw_begin(({op}) | (in_place ? GLW_SYNC : 0), {nw}, in_place ? 0 : bytes);')
        else:
            w(f'    uint32_t *a = glw_begin(({op}){" | GLW_SYNC" if sync else ""}, {nw}, 0);')
        k = 0
        for j, kd in enumerate(f['kinds']):
            if kd == 'w64':
                w(f'    a[{k}] = (uint32_t)p{j}; a[{k + 1}] = (uint32_t)(p{j} >> 32);')
                k += 2
            else:
                w(f'    a[{k}] = (uint32_t)p{j};')
                k += 1
        if f['cls'] == 'COPY':
            w('    if (!in_place) {')
            w(f'      uint8_t *d = (uint8_t *)(a + {nw});')
            for j, _ in sizes:
                w(f'      if (n{j}) memcpy(d, (const void *)p{j}, n{j});')
                w(f'      d += (n{j} + 3) & ~3u;')
            w('      glw_end(a);')
            w('    } else {')
            w('      (void)glw_sync(a);')
            w('    }')
        elif sync:
            w(f'    {"return " if f["ret"] else "(void)"}glw_sync(a);')
        else:
            w('    glw_end(a);')
        w('  }')
        w('}')
        w('')
    # replay
    w('/* The worker: run one recorded ASLgl call. */')
    w('void aslw_replay_gen(uint32_t idx, int sync, const uint32_t *a, uint32_t *ret) {')
    w('  switch (idx) {')
    for i, f in enumerate(fns):
        c = f['name']
        nw = sum(2 if k == 'w64' else 1 for k in f['kinds'])
        exprs = []
        k = 0
        if f['cls'] == 'COPY':
            w(f'  case {i}: {{')
            sizes = [(j, f['rule'][j]) for j, kd in enumerate(f['kinds']) if kd == 'in']
            # recompute sizes from the recorded args to find each copy
            for j, kd in enumerate(f['kinds']):
                if kd == 'w64':
                    w(f'    uint64_t p{j} = (uint64_t)a[{k}] | ((uint64_t)a[{k + 1}] << 32); (void)p{j};')
                    k += 2
                else:
                    w(f'    uint32_t p{j} = a[{k}]; (void)p{j};')
                    k += 1
            w(f'    const uint8_t *d = (const uint8_t *)(a + {nw});')
            for j, expr in sizes:
                w(f'    uint32_t n{j} = p{j} ? (uint32_t)({expr}) : 0;')
                w(f'    uintptr_t q{j} = !p{j} ? 0 : sync ? (uintptr_t)p{j} : (uintptr_t)d; if (!sync) d += (n{j} + 3) & ~3u;')
            call = ', '.join(f'q{j}' if kd == 'in' else f'p{j}' for j, kd in enumerate(f['kinds']))
            w(f'    o_{c}({call});')
            w('    break;')
            w('  }')
            continue
        for j, kd in enumerate(f['kinds']):
            if kd == 'w64':
                exprs.append(f'(uint64_t)a[{k}] | ((uint64_t)a[{k + 1}] << 32)')
                k += 2
            else:
                exprs.append(f'a[{k}]')
                k += 1
        call = f'o_{c}({", ".join(exprs)})'
        w(f'  case {i}: {"*ret = " if f["ret"] else ""}{call}; break;')
    w('  default: break;')
    w('  }')
    w('  (void)sync; (void)ret;')
    w('}')
    w('')
    w('const aslw_fn_t aslw_fns[] = {')
    for i, f in enumerate(fns):
        c = f['name']
        w(f'  {{ "{f["m"]}", "{c}", (void *)aslw_gen_{c}, (void **)&o_{c}, (void *)aslw_custom_{c} }},')
    w('};')
    w(f'const unsigned aslw_nfns = {len(fns)};')
    w('')
    w('#endif')
    open(out, 'w').write('\n'.join(L) + '\n')
    from collections import Counter
    print(len(fns), 'functions:', dict(Counter(f['cls'] for f in fns)))
    print('SYNC:', ' '.join(f['name'] for f in fns if f['cls'] == 'SYNC'))


if __name__ == '__main__':
    main()
