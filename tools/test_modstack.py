#!/usr/bin/env python3
"""test_modstack.py -- checks loader/modstack.c, built for the host by
tools/test_modstack.c, against a model of its rules written here.

    python3 tools/test_modstack.py <mods folder> <patch OBB> [language tag...]

<mods folder> is a card-style ux0:data/kotor2/mods/ (one folder per mod, as
shipped). The OBB is the Android patch OBB, for the talk tables. A scratch
card root is made in a temporary folder with links to both, so nothing is
written next to them. Every file of the view is resolved and compared, every
folder listed, and the composed talk table read back entry by entry.
"""
import os, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BUILTIN = {'english', 'french', 'german', 'italian', 'spanish'}
DLC = 'ux0:data/kotor2/dlc'


def obb_entries(path):
    with open(path, 'rb') as f:
        f.seek(-16, 2)
        toff, _ = struct.unpack('<QQ', f.read(16))
        f.seek(toff)
        raw = f.read()
    t = zlib.decompress(raw[:-16])
    n = struct.unpack_from('<Q', t, 0)[0]
    o, out = 8, {}
    for _ in range(n):
        ln = struct.unpack_from('<Q', t, o)[0]
        o += 8
        name = t[o:o + ln].decode('latin1')
        o += ln
        off, size, _ = struct.unpack_from('<QQQ', t, o)
        o += 24
        out[name.lower()] = (off, size)
    return out


def tlk(data):
    sig, lang, n, so = struct.unpack_from('<8sIII', data, 0)
    assert sig == b'TLK V3.0', sig
    ents = []
    for i in range(n):
        flags, snd, _, _, off, ln, _ = struct.unpack_from('<I16sIIIIf', data, 20 + 40 * i)
        ents.append((flags, snd, data[so + off:so + off + ln]))
    return lang, ents


def lang_dirs(d):
    try:
        return [e for e in os.listdir(d) if e.lower().startswith('mods_') and len(e) > 5 and os.path.isdir(os.path.join(d, e))]
    except OSError:
        return []


def find_root(base):
    """(root, loose) the way modstack.c's find_root picks it."""
    content = ('override', 'modules', 'lips', 'streamvoice', 'streammusic', 'streamsounds', 'movies', 'texturepacks', 'dialog.tlk')
    cands = [base]
    subs = [e for e in os.listdir(base) if not e.startswith('.') and os.path.isdir(os.path.join(base, e))]
    if len(subs) == 1:
        cands.append(os.path.join(base, subs[0]))
    for b in cands:
        for p in (b, os.path.join(b, 'dlc')):
            if lang_dirs(p):
                return p, False
        if any(os.path.exists(os.path.join(b, c)) for c in content):
            return b, True
    return None, None


def model(mods_dir, tag):
    mods = []
    for name in sorted((e for e in os.listdir(mods_dir) if not e.startswith('.') and os.path.isdir(os.path.join(mods_dir, e))),
                       key=str.lower):
        root, loose = find_root(os.path.join(mods_dir, name))
        if root:
            mods.append((name, root, loose))
    sources = []
    for name, root, loose in reversed(mods):
        if loose:
            sources.append(root)
            continue
        for t in ([tag] if tag == 'english' else [tag, 'english']):
            p = os.path.join(root, 'mods_' + t)
            if os.path.isdir(p):
                sources.append(p)
    index = {}            # lowercase rel -> (host path, is_dir)
    for src in sources:
        for dirpath, dirnames, filenames in os.walk(src):
            rel_dir = os.path.relpath(dirpath, src)
            rel_dir = '' if rel_dir == '.' else rel_dir
            for d in dirnames:
                r = os.path.join(rel_dir, d) if rel_dir else d
                index.setdefault(r.lower(), (os.path.join(dirpath, d), True))
            for f in filenames:
                if not rel_dir and f.lower() in ('dialog.tlk', 'dialogf.tlk'):
                    continue
                r = os.path.join(rel_dir, f) if rel_dir else f
                index.setdefault(r.lower(), (os.path.join(dirpath, f), False))
    return mods, sources, index


def run(driver, card, tag, folder, requests):
    env = dict(os.environ, HOST_UX0=card)
    p = subprocess.run([driver, tag, folder], input='\n'.join(requests) + '\n', capture_output=True, text=True, env=env, check=True)
    log = [l[2:] for l in p.stdout.splitlines() if l.startswith('# ')]
    return [l for l in p.stdout.splitlines() if not l.startswith('# ')], log


def host(card, vita_path):
    return os.path.join(card, vita_path[len('ux0:'):].lstrip('/'))


def check(driver, mods_dir, obb, tag):
    folder = tag if tag in BUILTIN else 'english'
    fails = 0
    with tempfile.TemporaryDirectory() as card:
        k2 = os.path.join(card, 'data', 'kotor2')
        os.makedirs(os.path.join(k2, 'dlc'))
        os.symlink(os.path.abspath(mods_dir), os.path.join(k2, 'mods'))
        os.symlink(os.path.abspath(obb), os.path.join(k2, 'patch.14.com.aspyr.swkotorii.obb'))
        mods, sources, index = model(mods_dir, tag)
        view = 'ux0:data/kotor2/dlc/mods_' + folder
        reqs = ['T']
        rels = sorted(index)
        reqs += ['R %s/%s' % (view, index[r][0][len(next(s for s in sources if index[r][0].startswith(s + '/'))) + 1:]) for r in rels]
        dirs = [''] + [r for r in rels if index[r][1]]
        reqs += ['L %s/%s' % (view, r) if r else 'L ' + view for r in dirs]
        odd = ['R ' + DLC, 'R ' + view, 'R ux0:/data/kotor2/./dlc//mods_%s/override/' % folder, 'R %s/no/such.file' % view,
               'R ux0:data/kotor2/dlc/english/modules', 'R ux0:data/kotor2/override/x.2da', 'L ' + DLC]
        if folder != 'english':
            odd.append('R ux0:data/kotor2/dlc/mods_english/override')
        out, log = run(driver, card, tag, folder, reqs + odd)
        it = iter(out)

        # the talk table
        t = next(it).split(' ', 2)
        tlk_line = ' '.join(t)
        tlks = []
        for name, root, loose in reversed(mods):
            if not loose:
                p = os.path.join(root, 'mods_%s' % tag, 'dialog.tlk')
                if os.path.isfile(p):
                    tlks.append(p)
        e = obb_entries(obb)
        with open(obb, 'rb') as f:
            off, size = e['localized/%s/dialog.tlk' % folder]
            f.seek(off)
            obb_lang, obb_ents = tlk(f.read(size))
        eng = []
        if tag != 'english':
            for name, root, loose in reversed(mods):
                p = os.path.join(root, 'dialog.tlk') if loose else os.path.join(root, 'mods_english', 'dialog.tlk')
                if os.path.isfile(p):
                    eng.append(tlk(open(p, 'rb').read()) + (p,))
        base_lang, base_ents = tlk(open(tlks[0], 'rb').read()) if tlks else (obb_lang, obb_ents)
        ext = max(eng, key=lambda x: len(x[1])) if eng else None
        if ext and len(ext[1]) > len(base_ents):
            want = base_ents + ext[1][len(base_ents):]
            if t[1] != '1' or not t[2].endswith('/cache/lang/%s/dialog.tlk' % tag):
                print('FAIL talk table: %s, wanted the composed cache' % tlk_line); fails += 1
            else:
                got_lang, got = tlk(open(host(card, t[2]), 'rb').read())
                bad = [i for i in range(max(len(got), len(want))) if i >= len(got) or i >= len(want) or got[i] != want[i]]
                if got_lang != base_lang or bad:
                    print('FAIL composed talk table: language %d (want %d), %d entries (want %d), %d differ, first %s'
                          % (got_lang, base_lang, len(got), len(want), len(bad), bad[:5])); fails += 1
                else:
                    print('ok   composed talk table: %d entries, language %d, %d English entries appended'
                          % (len(got), got_lang, len(want) - len(base_ents)))
        elif tlks:
            if t[1] != '1' or os.path.realpath(host(card, t[2])) != os.path.realpath(tlks[0]):
                print('FAIL talk table: %s, wanted %s' % (tlk_line, tlks[0])); fails += 1
            else:
                print('ok   talk table: the top-most mod\'s', tlks[0])
        else:
            if t[1] != '2' or t[2] != 'localized/%s/dialog.tlk' % folder:
                print('FAIL talk table: %s, wanted the OBB' % tlk_line); fails += 1
            else:
                print('ok   talk table: the OBB\'s localized/%s/dialog.tlk' % folder)

        # every file and folder of the view
        wrong = 0
        for r in rels:
            _, code, path = next(it).split(' ', 2)
            want_code = '2' if index[r][1] else '1'
            if code != want_code or os.path.realpath(host(card, path)) != os.path.realpath(index[r][0]):
                if wrong < 5:
                    print('FAIL %s -> %s %s, wanted %s %s' % (r, code, path, want_code, index[r][0]))
                wrong += 1
        print('%s %d of %d files and folders resolved to the right layer' % ('ok  ' if not wrong else 'FAIL', len(rels) - wrong, len(rels)))
        fails += wrong
        wrong = 0
        for r in dirs:
            names = []
            for line in it:
                if line == 'L.':
                    break
                names.append(line[2:])
            want = sorted(os.path.basename(k) + ('/' if v[1] else '') for k, v in index.items()
                          if os.path.dirname(k) == r)
            if not r and t[1] == '1':          # the chosen talk table sits at the top of the view
                want = sorted(want + ['dialog.tlk'])
            have = sorted(n.lower() for n in names)
            if have != want:
                if wrong < 3:
                    print('FAIL listing %r: %d names, wanted %d; extra %s missing %s' % (
                        r, len(have), len(want), sorted(set(have) - set(want))[:3], sorted(set(want) - set(have))[:3]))
                wrong += 1
        print('%s %d folders listed' % ('ok  ' if not wrong else 'FAIL', len(dirs)))
        fails += wrong

        # the edges
        expect = [('2', DLC), ('2', None), ('2', None), ('-1', ''), ('-1', ''), ('0', '')]
        for (want, _), req in zip(expect, odd):
            got = next(it).split(' ')
            if got[1] != want:
                print('FAIL %s -> %s, wanted %s' % (req, ' '.join(got[1:]), want)); fails += 1
        names = []
        for line in it:
            if line == 'L.':
                break
            names.append(line[2:])
        if names != ['mods_%s/' % folder]:
            print('FAIL dlc/ lists %s' % names); fails += 1
        if folder != 'english':
            got = next(it).split(' ')
            if got[1] != '-1':
                print('FAIL dlc/mods_english in %s -> %s' % (tag, got[1])); fails += 1
        if not fails:
            print('ok   dlc/, dlc/english, a missing file, a path outside dlc/, dlc/mods_english hidden')
        for l in log:
            if 'talk table' in l or 'indexed' in l:
                print('     ' + l)
    return fails


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    driver = os.path.join(tempfile.gettempdir(), 'test_modstack')
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-I', os.path.join(HERE, 'hoststubs', 'sceio'),
                    '-I', os.path.join(REPO, 'loader'), '-o', driver, os.path.join(HERE, 'test_modstack.c'),
                    os.path.join(REPO, 'loader', 'modstack.c'), os.path.join(REPO, 'loader', 'k2res.c'), '-lz'], check=True)
    fails = 0
    for tag in sys.argv[3:] or ['french', 'english']:
        print('== %s' % tag)
        fails += check(driver, sys.argv[1], sys.argv[2], tag)
    print('test_modstack: %s' % ('all checks passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
