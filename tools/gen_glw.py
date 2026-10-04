#!/usr/bin/env python3
"""Generate the GL worker interception layer (loader/glw_gen.c, cmake/glw_wraps.cmake).

Every vitaGL function the loader references (tools/glw_symbols.txt) is linked
with -Wl,--wrap so that all of the loader's calls, including addresses taken in
the import table, land in __wrap_<name>. Each wrapper records the call into the
GL worker's command stream (loader/gl_worker.c); the worker replays it into
__real_<name>. Kinds:
  SYNC    the caller needs a result or output: the worker runs it while the
          caller waits; pointer arguments are passed through unchanged.
  DATA    fire-and-forget with one input buffer copied into the stream.
  CUSTOM  hand-written in gl_worker.c (vertex arrays, draws, swap, state
          answered from the recorder's state copy, ...).
  plain   fire-and-forget with scalar arguments; pure state setters in
          DEDUP_GROUPS are skipped when they repeat the last call.
usage: gen_glw.py <vitaGL.h> <symbols.txt> <out.c> <out.cmake>
"""
import re, sys

SYNC = {
    'glCheckFramebufferStatus', 'glCreateProgram', 'glCreateShader', 'glGenBuffers', 'glGenFramebuffers',
    'glGenQueries', 'glGenRenderbuffers', 'glGenTextures', 'glGetActiveAttrib', 'glGetActiveUniform',
    'glGetAttachedShaders', 'glGetAttribLocation', 'glGetBooleanv', 'glGetBufferParameteriv', 'glGetError',
    'glGetFloatv', 'glGetFramebufferAttachmentParameteriv', 'glGetIntegerv', 'glGetProgramInfoLog',
    'glGetProgramiv', 'glGetQueryObjectuiv', 'glGetShaderInfoLog', 'glGetShaderSource', 'glGetShaderiv',
    'glGetString', 'glGetUniformLocation', 'glGetVertexAttribPointerv', 'glGetVertexAttribfv',
    'glGetVertexAttribiv', 'glIsEnabled', 'glIsFramebuffer', 'glIsProgram', 'glIsRenderbuffer', 'glIsTexture',
    'glMapBuffer', 'glUnmapBuffer', 'glReadPixels', 'glFinish', 'glShaderSource', 'glShaderBinary',
    'vglInit', 'vglInitExtended', 'vglSetupRuntimeShaderCompiler',
}
CUSTOM = {
    'glVertexAttribPointer', 'glVertexPointer', 'glTexCoordPointer', 'glEnableVertexAttribArray',
    'glDisableVertexAttribArray', 'glEnableClientState', 'glDisableClientState', 'glBindBuffer',
    'glDeleteBuffers', 'glDrawArrays', 'glDrawElements', 'glPixelStorei', 'glUseProgram', 'vglSwapBuffers',
    'vglGetProcAddress',
    # answered on the producer side from a state copy (gl_worker.c)
    'glIsProgram', 'glIsEnabled', 'glGetIntegerv', 'glGetError', 'glGetQueryObjectuiv', 'glBeginQuery',
    'glEndQuery', 'glGenQueries', 'glCreateProgram', 'glDeleteProgram', 'glEnable', 'glDisable',
    'glBindFramebuffer', 'glScissor', 'glViewport',
}
# DATA: name -> (pointer parameter, byte-size expression over the parameters)
DATA = {
    'glBindAttribLocation': ('name', 'strlen(name) + 1'),
    'glBufferData': ('data', 'size'),
    'glBufferSubData': ('data', 'size'),
    'glCompressedTexImage2D': ('data', 'imageSize'),
    'glDeleteFramebuffers': ('framebuffers', 'n * 4'),
    'glDeleteRenderbuffers': ('renderbuffers', 'n * 4'),
    'glDeleteTextures': ('textures', 'n * 4'),
    'glTexImage2D': ('data', 'glw_pixels_size(width, height, format, type)'),
    'glTexSubImage2D': ('pixels', 'glw_pixels_size(width, height, format, type)'),
    'glTexParameteriv': ('param', '16'),
    'glUniform1fv': ('value', 'count * 4'), 'glUniform2fv': ('value', 'count * 8'),
    'glUniform3fv': ('value', 'count * 12'), 'glUniform4fv': ('value', 'count * 16'),
    'glUniform1iv': ('value', 'count * 4'), 'glUniform2iv': ('value', 'count * 8'),
    'glUniform3iv': ('value', 'count * 12'), 'glUniform4iv': ('value', 'count * 16'),
    'glUniformMatrix2fv': ('value', 'count * 16'), 'glUniformMatrix3fv': ('value', 'count * 36'),
    'glUniformMatrix4fv': ('value', 'count * 64'),
    'glVertexAttrib1fv': ('v', '4'), 'glVertexAttrib2fv': ('v', '8'),
    'glVertexAttrib3fv': ('v', '12'), 'glVertexAttrib4fv': ('v', '16'),
}
FLOATS = {'GLfloat', 'GLclampf', 'float'}
# Pure state setters skipped when called again with the same arguments
# (glw_dedup in gl_worker.c). Ops in one group set the same state, so recording
# one forgets the others' last arguments.
DEDUP_GROUPS = [
    ['glBlendFunc', 'glBlendFuncSeparate'], ['glBlendEquation', 'glBlendEquationSeparate'],
    ['glDepthFunc'], ['glDepthMask'], ['glDepthRangef'], ['glCullFace'], ['glFrontFace'], ['glColorMask'],
    ['glStencilFunc', 'glStencilFuncSeparate'], ['glStencilOp', 'glStencilOpSeparate'],
    ['glStencilMask', 'glStencilMaskSeparate'], ['glPolygonOffset'], ['glLineWidth'], ['glClearColor'],
    ['glClearDepthf'], ['glClearStencil'], ['glActiveTexture'],
]
DEDUP = {op: gi for gi, g in enumerate(DEDUP_GROUPS) for op in g}

hdr, symf, outc, outcm = sys.argv[1:5]
text = open(hdr).read()
syms = [s.strip() for s in open(symf) if s.strip() and s.strip() not in ('gxm_shader_patcher', 'is_shark_online')]

protos = {}
for s in syms:
    m = re.search(r'^\s*(?:extern\s+)?([A-Za-z_][\w\s\*]*?[\s\*])' + re.escape(s) + r'\s*\(([^)]*)\)\s*;', text, re.M)
    if not m:
        sys.exit('no prototype for ' + s)
    ret = ' '.join(m.group(1).split())
    params = [p.strip() for p in m.group(2).split(',')] if m.group(2).strip() not in ('', 'void') else []
    plist = []
    for p in params:
        pm = re.match(r'^(.*?)([A-Za-z_]\w*)$', p)
        if not pm:
            sys.exit('bad param %r in %s' % (p, s))
        ptype = ' '.join(pm.group(1).replace('*', ' * ').split()).replace(' *', '*').replace('* ', '*')
        plist.append((ptype, pm.group(2)))
    protos[s] = (ret, plist)

def is_ptr(t): return '*' in t
def is_float(t): return t.replace('const ', '') in FLOATS

ops = [s for s in syms]
out = []
w = out.append
w('/* glw_gen.c -- GENERATED by tools/gen_glw.py; do not edit. */')
w('#include "gl_worker_internal.h"')
w('')
w('enum {')
for i, s in enumerate(ops):
    w('  GLW_OP_%s = %d,' % (s, i + 1))
w('  GLW_OP_GEN_COUNT')
w('};')
w('_Static_assert(GLW_OP_GEN_COUNT <= GLW_OP_CUSTOM_BASE, "generated ops overlap custom ops");')
w('')
w('const char *const glw_op_names[GLW_OP_GEN_COUNT] = {')
w('  "?",')
for s in ops:
    w('  "%s",' % s)
w('};')
w('')
for s in ops:
    ret, pl = protos[s]
    decl = ', '.join('%s %s' % (t, n) for t, n in pl) or 'void'
    w('extern %s __real_%s(%s);' % (ret, s, decl))
w('')
for s in ops:
    if s in CUSTOM:
        continue
    ret, pl = protos[s]
    decl = ', '.join('%s %s' % (t, n) for t, n in pl) or 'void'
    names = ', '.join(n for t, n in pl)
    ptrs = [n for t, n in pl if is_ptr(t)]
    kind = 'SYNC' if s in SYNC else 'DATA' if s in DATA else 'plain'
    if kind == 'plain' and ptrs:
        sys.exit('%s has pointer parameters %s but no kind' % (s, ptrs))
    if kind == 'DATA':
        dp, dsz = DATA[s]
        if dp not in [n for t, n in pl]:
            sys.exit('%s: no parameter %s' % (s, dp))
        others = [p for p in ptrs if p != dp]
        if others:
            sys.exit('%s: unhandled pointers %s' % (s, others))
    isvoid = ret == 'void'
    w('%s __wrap_%s(%s) {' % (ret, s, decl))
    w('  GLW_PROF(1);')
    if isvoid:
        w('  if (glw_direct()) { __real_%s(%s); return; }' % (s, names))
    else:
        w('  if (glw_direct()) return __real_%s(%s);' % (s, names))
    n = len(pl)
    def enc(t, nme):
        if is_float(t): return 'glw_u(%s)' % nme
        if is_ptr(t): return '(uint32_t)(uintptr_t)%s' % nme
        return '(uint32_t)%s' % nme
    if kind == 'SYNC':
        w('  uint32_t *a = glw_begin(GLW_OP_%s | GLW_SYNC, %d, 0);' % (s, n))
        for i, (t, nme) in enumerate(pl):
            w('  a[%d] = %s;' % (i, enc(t, nme)))
        if isvoid:
            w('  (void)glw_sync(a);')
        else:
            w('  return (%s)(uintptr_t)glw_sync(a);' % ret)
    elif kind == 'DATA':
        dp, dsz = DATA[s]
        w('  uint32_t bytes = %s ? (uint32_t)(%s) : 0;' % (dp, dsz))
        w('  if (bytes > GLW_INLINE_MAX) {')
        w('    uint32_t *a = glw_begin(GLW_OP_%s | GLW_SYNC, %d, 0);' % (s, n))
        for i, (t, nme) in enumerate(pl):
            w('    a[%d] = %s;' % (i, enc(t, nme)))
        w('    (void)glw_sync(a);')
        w('    return;')
        w('  }')
        w('  uint32_t *a = glw_begin(GLW_OP_%s, %d, bytes);' % (s, n))
        for i, (t, nme) in enumerate(pl):
            if nme == dp:
                w('  a[%d] = bytes ? 1u : 0u;' % i)
            else:
                w('  a[%d] = %s;' % (i, enc(t, nme)))
        w('  if (bytes) memcpy(a + %d, %s, bytes);' % (n, dp))
        w('  glw_end(a);')
    else:
        if s in DEDUP and 0 < n <= 4:
            w('  uint32_t v[%d] = {%s};' % (n, ', '.join(enc(t, nme) for t, nme in pl)))
            w('  if (glw_dedup(%d, GLW_OP_%s, v, %d)) return;' % (DEDUP[s], s, n))
        w('  uint32_t *a = glw_begin(GLW_OP_%s, %d, 0);' % (s, n))
        for i, (t, nme) in enumerate(pl):
            w('  a[%d] = %s;' % (i, enc(t, nme)))
        w('  glw_end(a);')
    w('}')
    w('')

# Replay: inline data follows the arguments for DATA calls; SYNC calls pass raw pointers.
w('int glw_replay_gen(uint32_t op, int sync, const uint32_t *a, uint32_t *ret) {')
w('  switch (op) {')
for s in ops:
    if s in CUSTOM:
        continue
    ret, pl = protos[s]
    n = len(pl)
    args = []
    for i, (t, nme) in enumerate(pl):
        if s in DATA and nme == DATA[s][0]:
            args.append('(%s)(sync ? (const void *)(uintptr_t)a[%d] : (a[%d] ? (const void *)(a + %d) : NULL))' % (t, i, i, n))
        elif is_float(t):
            args.append('glw_f(a[%d])' % i)
        elif is_ptr(t):
            args.append('(%s)(uintptr_t)a[%d]' % (t, i))
        else:
            args.append('(%s)a[%d]' % (t, i))
    call = '__real_%s(%s)' % (s, ', '.join(args))
    if ret == 'void':
        w('  case GLW_OP_%s: %s; *ret = 0; return 1;' % (s, call))
    else:
        w('  case GLW_OP_%s: *ret = (uint32_t)(uintptr_t)%s; return 1;' % (s, call))
w('  default: return 0;')
w('  }')
w('}')
w('')
# Name table for vglGetProcAddress: every wrapped function, so the game never
# gets a raw vitaGL pointer that would bypass the worker.
for s in ops:
    if s in CUSTOM:
        ret, pl = protos[s]
        w('extern %s __wrap_%s(%s);' % (ret, s, ', '.join('%s %s' % (t, n) for t, n in pl) or 'void'))
w('const glw_proc_t glw_procs[] = {')
for s in ops:
    if s.startswith('gl'):
        w('  {"%s", (void *)__wrap_%s},' % (s, s))
w('  {NULL, NULL},')
w('};')
open(outc, 'w').write('\n'.join(out) + '\n')

cm = ['# GENERATED by tools/gen_glw.py; do not edit.', 'set(GLW_WRAP_FLAGS']
for s in ops:
    cm.append('  "-Wl,--wrap=%s"' % s)
cm.append(')')
open(outcm, 'w').write('\n'.join(cm) + '\n')
print('generated %d wrappers (%d sync, %d data, %d custom, %d plain, %d dedup)' % (
    len(ops), len([s for s in ops if s in SYNC and s not in CUSTOM]), len([s for s in ops if s in DATA]),
    len([s for s in ops if s in CUSTOM]), len([s for s in ops if s not in SYNC | set(DATA) | CUSTOM]),
    len([s for s in ops if s in DEDUP])))
