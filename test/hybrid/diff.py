"""Decodes the in-game dumps written by test/hybrid/hybrid.lua and diffs them pixel for pixel:
  - front frames 1 + 2 against the owner's reference PNGs (REFDIR/<name>.png, <name>_2.png; modern_hybrids.py output);
  - back sprites against tools/hybrid/hybrid_ref.py (no owner back references yet).
Also writes OUT/<name>[_2|_back].png (the ROM's render) and OUT/diff_<name>.png (red = differing pixel) when a check fails.
usage: diff.py OUTDIR REFDIR"""
import struct
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'hybrid'))
import hybrid_lib as L
import hybrid_table as T

OUT, REF = Path(sys.argv[1]), Path(sys.argv[2])


def decode(tiles, frame, pal):
    im = Image.new('RGBA', (64, 64))
    for y in range(64):
        for x in range(64):
            yy = y + frame * 64
            b = tiles[((yy >> 3) * 8 + (x >> 3)) * 32 + (yy & 7) * 4 + ((x & 7) >> 1)]
            v = (b >> 4) if (x & 1) else (b & 15)
            if v:
                c = pal[v]
                im.putpixel((x, y), L.expand((c & 31, (c >> 5) & 31, (c >> 10) & 31)) + (255,))
    return im


def diff(a, b):
    pa, pb = a.convert('RGBA').load(), b.convert('RGBA').load(); bad = []
    for y in range(64):
        for x in range(64):
            ca, cb = pa[x, y], pb[x, y]
            if (ca[3] == 0) != (cb[3] == 0) or (ca[3] and ca[:3] != cb[:3]): bad.append((x, y))
    return bad


def main():
    cases = {c[0]: c for c in T.CASES}
    fails = checks = 0
    names = sorted(p.stem for p in OUT.glob('*.bin'))
    if not names: sys.exit('no dumps: the ROM test did not reach the hybrid stage')
    for name in names:
        raw = (OUT / f'{name}.bin').read_bytes()
        pal = struct.unpack_from('<16H', raw, 40)
        front, back = raw[72:72 + 4096], raw[72 + 4096:72 + 4096 + 2048]
        got = {'': decode(front, 0, pal), '_2': decode(front, 1, pal), '_back': decode(back, 0, pal)}
        want = {}
        for suf in ('', '_2'):
            if (REF / f'{name}{suf}.png').exists(): want[suf] = Image.open(REF / f'{name}{suf}.png')
        if name in cases:
            _, body, colour, parts = cases[name]
            views, palette = L.compose(body, colour, parts)
            want['_back'] = L.to_png(views['back'], palette)
        for suf, im in got.items():
            im.save(OUT / f'{name}{suf}.png')
            if suf not in want: continue
            checks += 1
            bad = diff(im, want[suf])
            src = 'hybrid_ref.py' if suf == '_back' else 'reference'
            print(f'  {"ok  " if not bad else "FAIL"}  {name}{suf} vs {src}' + (f': {len(bad)} px differ' if bad else ''))
            if bad:
                fails += 1
                d = Image.new('RGBA', (64, 64))
                d.alpha_composite(im)
                for p in bad: d.putpixel(p, (255, 0, 0, 255))
                d.resize((256, 256), Image.Resampling.NEAREST).save(OUT / f'diff_{name}{suf}.png')
    print(f'{checks} sprite checks, {fails} failed')
    sys.exit(1 if fails or checks == 0 else 0)


if __name__ == '__main__':
    main()
