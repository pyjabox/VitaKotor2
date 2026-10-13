#!/usr/bin/env python3
"""voicepack.py -- a voice pack for the Vita from a PC copy of KOTOR II.

    python3 tools/voicepack.py <PC game folder or zip> <patch OBB> <language> <out>

The Android release ships voices, lip sync and voiced movies in English only,
under localized/english/ in the patch OBB. The engine looks each of those
files up under localized/<language>/ first, so copying a dubbed PC version's
files to ux0:data/kotor2/localized/<language>/ gives the game that language's
voices wherever the pack has the line, and English wherever it does not.

This copies from the PC game (its folder, or a zip of it -- one zip nested in
another is opened too) exactly the files the OBB has under localized/english/:
  StreamVoice/   -> streamvoice/   the dialogue
  lips/*_loc.mod -> lips/          lip sync
  StreamSounds/  -> streamsounds/  the barks the OBB keeps per language
  Movies/        -> movies/        the voiced movies (kremov01, scnmov01, trailer)
Files that are byte-identical to the English ones are left out (the game
falls back to English for them anyway), as are files the OBB does not have
there (mods' additions: mods bring their own). Every audio file's container is
checked against the three the Vita loader decodes (loader/audio_mp3.c).

<language> is the folder name the engine uses: french, german, italian or
spanish. <out> receives localized/<language>/...; copy that folder to
ux0:data/kotor2/ on the Vita.
"""
import collections, hashlib, io, os, shutil, struct, sys, zipfile, zlib

CATEGORIES = ('streamvoice', 'lips', 'streamsounds', 'movies')


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


class Source:
    """The PC game's files by lowercase relative path, from a folder or a zip."""

    def __init__(self, path):
        self.files = {}
        if os.path.isdir(path):
            for dp, _, fn in os.walk(path):
                for f in fn:
                    full = os.path.join(dp, f)
                    self.files[os.path.relpath(full, path).replace(os.sep, '/').lower()] = ('fs', full)
            return
        z = zipfile.ZipFile(path)
        names = [i for i in z.infolist() if not i.is_dir()]
        inner = [i for i in names if i.filename.lower().endswith('.zip')]
        if len(inner) == 1 and not any(self._category(i.filename) for i in names):
            z = zipfile.ZipFile(z.open(inner[0]))     # a zip of the zip
            names = [i for i in z.infolist() if not i.is_dir()]
        self.zip = z
        for i in names:
            self.files[i.filename.lower()] = ('zip', i)
        # The game folder may itself be one level down in the archive.
        prefixes = collections.Counter(k.split('/', 1)[0] for k in self.files if '/' in k)
        if not any(self._category(k) for k in self.files):
            for p, _ in prefixes.most_common(1):
                self.files = {k[len(p) + 1:]: v for k, v in self.files.items() if k.startswith(p + '/')}

    @staticmethod
    def _category(name):
        return name.lower().split('/', 1)[0] in CATEGORIES

    def size(self, key):
        kind, ref = self.files[key]
        return os.path.getsize(ref) if kind == 'fs' else ref.file_size

    def read(self, key, n=None):
        kind, ref = self.files[key]
        if kind == 'fs':
            with open(ref, 'rb') as f:
                return f.read() if n is None else f.read(n)
        with self.zip.open(ref) as f:
            return f.read() if n is None else f.read(n)

    def copy_to(self, key, dest):
        kind, ref = self.files[key]
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if kind == 'fs':
            shutil.copyfile(ref, dest)
        else:
            with self.zip.open(ref) as src, open(dest, 'wb') as out:
                shutil.copyfileobj(src, out, 1 << 20)


def container(head):
    """Which of audio_mp3.c's three layouts a voice or bark file uses."""
    if head[:4] == b'RIFF' and len(head) > 60 and head[58] == 0xff and head[59] & 0xe0 == 0xe0:
        return 'MP3 behind a 58-byte RIFF header'
    if head[:4] == b'RIFF':
        return 'RIFF'
    if len(head) > 474 and head[470:474] == b'RIFF':
        return 'RIFF behind a 470-byte MP3 header'
    if head[:3] == b'ID3' or (len(head) > 1 and head[0] == 0xff and head[1] & 0xe0 == 0xe0):
        return 'MP3'
    return None


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    src_path, obb_path, lang, out = sys.argv[1:]
    lang = lang.lower()
    if lang not in ('french', 'german', 'italian', 'spanish'):
        print('language must be french, german, italian or spanish (the engine has no other)')
        return 2
    obb = obb_entries(obb_path)
    eng = {k[len('localized/english/'):]: v for k, v in obb.items() if k.startswith('localized/english/')}
    want = {k: v for k, v in eng.items() if k.split('/', 1)[0] in CATEGORIES and not k.endswith('.txt')}
    have_lang = {k[len('localized/%s/' % lang):] for k in obb if k.startswith('localized/%s/' % lang)}

    src = Source(src_path)
    obbf = open(obb_path, 'rb')
    stats = collections.defaultdict(collections.Counter)
    formats = collections.Counter()
    odd = []
    dest_root = os.path.join(out, 'localized', lang)
    for key in sorted(want):
        cat = key.split('/', 1)[0]
        if key not in src.files:
            stats[cat]['not in the PC copy'] += 1
            continue
        if key in have_lang:
            stats[cat]['already in the OBB for this language'] += 1
            continue
        if cat in ('streamvoice', 'streamsounds'):
            fmt = container(src.read(key, 600))
            formats[fmt] += 1
            if not fmt:
                odd.append(key)
                stats[cat]['unknown audio format: left out'] += 1
                continue
        if src.size(key) == want[key][1]:
            obbf.seek(want[key][0])
            if hashlib.sha1(obbf.read(want[key][1])).digest() == hashlib.sha1(src.read(key)).digest():
                stats[cat]['identical to English: left out'] += 1
                continue
        src.copy_to(key, os.path.join(dest_root, key))
        stats[cat]['copied'] += 1
        stats[cat]['bytes'] += src.size(key)

    extra = sorted(k for k in src.files if k.split('/', 1)[0] in CATEGORIES and k not in want)
    print('voice pack for %s in %s' % (lang, dest_root))
    for cat in CATEGORIES:
        s = stats[cat]
        parts = ['%d copied (%.1f MB)' % (s['copied'], s['bytes'] / 1e6)]
        parts += ['%d %s' % (v, k) for k, v in sorted(s.items()) if k not in ('copied', 'bytes')]
        print('  %-13s %s' % (cat + '/', ', '.join(parts)))
    print('  audio containers: ' + ', '.join('%d %s' % (v, k or 'unknown') for k, v in formats.most_common()))
    if odd:
        print('  unknown format, e.g. ' + ', '.join(odd[:5]))
    print('  %d PC files the OBB does not keep per language were left out (mods bring their own), e.g. %s'
          % (len(extra), ', '.join(extra[:3])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
