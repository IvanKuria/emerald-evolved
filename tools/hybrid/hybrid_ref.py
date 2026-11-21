"""Reference renderer: the hybrid sprites exactly as the ROM draws them (same table, same merge as src/hybrid_sprite.c).
usage (from the repo root):
  python3 tools/hybrid/hybrid_ref.py <out dir> [--sheet]     writes <name>.png (frame 1), <name>_2.png (frame 2),
                                                             <name>_back.png per hybrid_table.CASES (64x64 RGBA, 1x)
  python3 tools/hybrid/hybrid_ref.py --check <canonical dir> diffs frames 1 + 2 against the owner's PNGs
                                                             (modern_hybrids.py output); exit 1 on any pixel difference"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hybrid_table as T
import hybrid_lib as L

SUFFIX = {'f1': '', 'f2': '_2', 'back': '_back'}


def renders():
    for name, body, colour, parts in T.CASES:
        res = L.compose(body, colour, parts)
        if res is None: raise SystemExit(f'{name}: this look draws the plain species')
        views, palette = res
        for view, rows in views.items():
            yield f'{name}{SUFFIX[view]}', view, L.to_png(rows, palette)


def diff(a, b):
    """number of pixels that differ (RGBA, alpha 0 counts as one 'transparent' value)"""
    pa, pb = a.convert('RGBA').load(), b.convert('RGBA').load(); n = 0
    for y in range(64):
        for x in range(64):
            ca, cb = pa[x, y], pb[x, y]
            if (ca[3] == 0) != (cb[3] == 0) or (ca[3] and ca[:3] != cb[:3]): n += 1
    return n


def main():
    if sys.argv[1] == '--check':
        ref = Path(sys.argv[2]); bad = 0
        for name, view, im in renders():
            if view == 'back': continue
            d = diff(im, Image.open(ref / f'{name}.png'))
            print(f'{name:24s} {"ok" if d == 0 else f"DIFF {d} px"}'); bad += d != 0
        sys.exit(1 if bad else 0)
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True); names = []
    for name, _, im in renders():
        im.save(out / f'{name}.png'); names.append(name)
    print(f'{len(names)} sprites -> {out}')
    if '--sheet' in sys.argv:
        K, cell, cols = 4, 64 * 4 + 16, 3
        sheet = Image.new('RGB', (cols * cell, ((len(names) + cols - 1) // cols) * (cell + 14)), (205, 205, 180))
        d = ImageDraw.Draw(sheet)
        for i, n in enumerate(names):
            im = Image.open(out / f'{n}.png').resize((64 * K, 64 * K), Image.Resampling.NEAREST)
            x, y = (i % cols) * cell + 8, (i // cols) * (cell + 14) + 8
            sheet.paste(im, (x, y), im); d.text((x, y + 64 * K + 2), n, fill=(0, 0, 0))
        sheet.save(out / 'contact_sheet.png')


if __name__ == '__main__':
    main()
