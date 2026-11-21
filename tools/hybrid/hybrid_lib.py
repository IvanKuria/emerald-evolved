"""Shared code for bake.py (writes the ROM's tables) and hybrid_ref.py (reference renderer). The compositing and the
15-colour merge here are a port of the owner's modern_hybrids.py and the SPEC that src/hybrid_sprite.c implements step for
step; test/hybrid/run.sh checks all three agree.

Colours are GBA RGB555 tuples (r, g, b in 0..31): the .pal 8-bit values >> 3, as gbagfx and modern_hybrids.merge15 do."""
from pathlib import Path
from PIL import Image

import hybrid_table as T

ROOT = Path(__file__).resolve().parents[2]
G = ROOT / 'graphics' / 'pokemon'
FRONT = ('f1', 'f2')
VIEWS = ('f1', 'f2', 'back')
MAX_PART_COLOURS = 32                      # HYBRID_MAX_PART_COLOURS in include/hybrid_sprite.h


def pal(sp, shiny=False):
    lines = open(G / sp / ('shiny.pal' if shiny else 'normal.pal')).read().split('\n')[3:19]
    return [tuple(int(v) >> 3 for v in l.split()) for l in lines]


def pal8(sp):
    return [tuple(map(int, l.split())) for l in open(G / sp / 'normal.pal').read().split('\n')[3:19]]


def view_image(sp, view):
    if view == 'back':
        return Image.open(G / sp / 'back.png').crop((0, 0, 64, 64))
    y = 0 if view == 'f1' else 64
    return Image.open(G / sp / 'anim_front.png').crop((0, y, 64, y + 64))


def view_pixels(sp, view):
    px = view_image(sp, view).load()
    return [[px[x, y] for x in range(64)] for y in range(64)]


# ---------------------------------------------------------------- ramps
def _hue(c):
    r, g, b = c; mx, mn = max(c), min(c)
    if mx == mn: return 0.0
    d = mx - mn
    if mx == r: h = ((g - b) / d) % 6
    elif mx == g: h = (b - r) / d + 2
    else: h = (r - g) / d + 4
    return h * 60.0


def _luma(c): return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def auto_ramp(sp):
    """ramp for species without a hand one: the most-used saturated colour of front frame 1 and every used colour
    within 30 degrees of its hue, light -> dark (at most 6)"""
    p = pal8(sp); cnt = [0] * 16
    for row in view_pixels(sp, 'f1'):
        for i in row: cnt[i] += 1
    cand = [i for i in range(1, 16) if cnt[i] and max(p[i]) - min(p[i]) >= 40 and 30 <= _luma(p[i]) <= 240]
    if not cand: return []
    seed = max(cand, key=lambda i: (cnt[i], -i))
    fam = [i for i in cand if min(abs(_hue(p[i]) - _hue(p[seed])), 360 - abs(_hue(p[i]) - _hue(p[seed]))) <= 30]
    fam = sorted(fam, key=lambda i: (-cnt[i], i))[:6]
    return sorted(fam, key=lambda i: (-_luma(p[i]), i))


def ramp(sp):
    if sp in T.RAMPS: return list(T.RAMPS[sp])
    if sp in T.RAMP_SPECIES: return auto_ramp(sp)
    return []


def ramp_map(body, colour_sp):
    """{body palette index: colour species palette index}: the owner's map if there is one, else body ramp role i ->
    colour ramp role min(i, len - 1). Same as HybridRampMap() in C (bake.py writes the owner maps as a table)."""
    if not colour_sp or colour_sp == body: return {}
    if (body, colour_sp) in T.RAMP_MAPS: return dict(T.RAMP_MAPS[(body, colour_sp)])
    br, cr = ramp(body), ramp(colour_sp)
    if not br or not cr: return {}
    return {b: cr[min(i, len(cr) - 1)] for i, b in enumerate(br)}


# ---------------------------------------------------------------- parts (baked with PIL, same calls as modern_hybrids.py)
def shift(sp, region, r=6):
    """modern_hybrids.shift: how far a region of frame 1 moved in frame 2 (best exact-index match, first best wins)"""
    A, B = view_image(sp, 'f1').load(), view_image(sp, 'f2').load(); x0, y0, x1, y1 = region; best = None
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            s = 0
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    xx, yy = x + dx, y + dy
                    v = B[xx, yy] if 0 <= xx < 64 and 0 <= yy < 64 else 0
                    s += (A[x, y] == v) and A[x, y] != 0
            if best is None or s > best[0]: best = (s, dx, dy)
    return best[1], best[2]


def _cut_image(part_sp, c):
    """'L' image, value = palette index + 1 (0 = transparent), so PIL's NEAREST rotate samples it exactly as it samples
    modern_hybrids' RGBA cut; cropped to its pixels like modern_hybrids.part"""
    src = view_pixels(part_sp, 'f1'); x0, y0, x1, y1 = c['box']
    im = Image.new('L', (64, 64))
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if src[y][x] in c['idx']: im.putpixel((x, y), src[y][x] + 1)
    return im.crop(im.getbbox())


def _placed(part_sp, p):
    """(image, top-left) in frame 1, as modern_hybrids composites it"""
    im = _cut_image(part_sp, p['src'])
    if p['angle']: im = im.rotate(p['angle'], resample=Image.NEAREST, expand=True)
    ax, ay = p['at']
    if p['mode'] == 'root':
        b = im.getbbox()
        return im, (ax - (b[0] + b[2]) // 2, ay - b[3])
    return im, (ax, ay)


def layers(body, part_sp):
    """[(view, behind, x0, y0, w, h, rows (0 = none, k = colours[k - 1]), colours)] in draw order: frame 1 pieces, then
    frame 2 pieces (shifted with their body region). Exactly what bake.py writes for the ROM."""
    out = []
    pieces = T.PLACEMENTS.get((body, part_sp), [])
    ppal = pal(part_sp)
    for view in FRONT:
        for p in pieces:
            im, (x, y) = _placed(part_sp, p)
            if view == 'f2':
                dx, dy = shift(body, p['region']); x += dx; y += dy
            bb = im.getbbox(); im = im.crop(bb); x += bb[0]; y += bb[1]
            # clip to the 64x64 frame, as alpha_composite onto a 64x64 canvas does
            cx0, cy0 = max(0, -x), max(0, -y)
            cx1, cy1 = min(im.width, 64 - x), min(im.height, 64 - y)
            if (cx0, cy0, cx1, cy1) != (0, 0, im.width, im.height):
                print(f'note: {body} x {part_sp} {view}: piece clipped by the frame')
                im = im.crop((cx0, cy0, cx1, cy1)); x += cx0; y += cy0
            px = im.load(); colours = []; rows = []
            for yy in range(im.height):
                row = []
                for xx in range(im.width):
                    v = px[xx, yy]
                    if v == 0: row.append(0); continue
                    c = ppal[v - 1]
                    if c not in colours: colours.append(c)
                    row.append(colours.index(c) + 1)
                rows.append(row)
            out.append((view, p['behind'], x, y, im.width, im.height, rows, colours))
    return out


# ---------------------------------------------------------------- the 15-colour merge (= modern_hybrids.merge15)
def ckey(c): return c                       # colours sort as (r, g, b) tuples; C: (r << 10) | (g << 5) | b


def merge15(count):
    """count: {colour: pixels}. -> {colour: surviving colour}: while more than 15 colours, merge the closest pair
    (squared RGB555 distance; ties -> the pair whose smaller (count, r, g, b) is lowest, then the first pair in sorted
    order) into whichever of the two has the larger (count, r, g, b)."""
    rep = {c: c for c in count}; live = dict(count)
    while len(live) > 15:
        cs = sorted(live); best = None
        for i in range(len(cs)):
            for j in range(i + 1, len(cs)):
                a, b = cs[i], cs[j]; dd = sum((a[k] - b[k]) ** 2 for k in range(3))
                key = (dd, min((live[a],) + a, (live[b],) + b))
                if best is None or key < best[0]: best = (key, a, b)
        _, a, b = best
        keep, drop = (a, b) if (live[a], a) >= (live[b], b) else (b, a)
        live[keep] += live.pop(drop)
        for c in rep:
            if rep[c] == drop: rep[c] = keep
    return rep, sorted(live)


# ---------------------------------------------------------------- the compositor (= src/hybrid_sprite.c)
def compose(body, colour_sp, part_sps, shiny=False):
    """-> ({view: 64x64 rows of palette indices}, 16-colour RGB555 palette), or None when the look changes nothing."""
    rmap = ramp_map(body, colour_sp)
    lays = [l for p in part_sps for l in layers(body, p)]
    if not rmap and not lays: return None
    cpal = pal(colour_sp) if rmap else None

    def virtual(b):
        v = list(b)
        for bi, ci in rmap.items(): v[bi] = cpal[ci]
        return v

    Vn, Vs = virtual(pal(body)), virtual(pal(body, shiny))
    pcols, lay_map = [], []
    for l in lays:
        m = []
        for c in l[7]:
            if c not in pcols:
                if len(pcols) == MAX_PART_COLOURS: raise SystemExit('too many part colours')
                pcols.append(c)
            m.append(16 + pcols.index(c))
        lay_map.append(m)
    Vn += pcols; Vs += pcols

    bufs = {}
    for view in VIEWS:
        bp = view_pixels(body, view); buf = [[0] * 64 for _ in range(64)]
        for behind_pass in (True, False):
            if not behind_pass:
                for y in range(64):
                    for x in range(64):
                        if bp[y][x]: buf[y][x] = bp[y][x]
            for l, m in zip(lays, lay_map):
                lv, behind, x0, y0, w, h, rows, _ = l
                if lv != view or behind != behind_pass: continue
                for yy in range(h):
                    for xx in range(w):
                        if rows[yy][xx]: buf[y0 + yy][x0 + xx] = m[rows[yy][xx] - 1]
        bufs[view] = buf

    count = {}
    for view in FRONT:
        for row in bufs[view]:
            for v in row:
                if v: count[Vn[v]] = count.get(Vn[v], 0) + 1
    rep, survivors = merge15(count)
    index = {c: k + 1 for k, c in enumerate(survivors)}

    def final(v):
        c = Vn[v]
        if c in rep: return index[rep[c]]
        # a colour only the back view uses: the nearest surviving colour (ties -> the lowest colour)
        return index[min(survivors, key=lambda s: (sum((s[k] - c[k]) ** 2 for k in range(3)), s))]

    # palette: each surviving colour, taken (for shiny) from the lowest virtual slot with that normal colour
    palette = [pal(body, shiny)[0]]
    for s in survivors:
        v = min(i for i in range(len(Vn)) if i > 0 and Vn[i] == s)
        palette.append(Vs[v])
    palette += [(0, 0, 0)] * (16 - len(palette))
    out = {view: [[final(v) if v else 0 for v in row] for row in buf] for view, buf in bufs.items()}
    return out, palette


def expand(c):
    """RGB555 -> 8-bit, as modern_hybrids.merge15 writes it and mGBA shows it"""
    return tuple((v << 3) | (v >> 2) for v in c)


def to_png(rows, palette):
    im = Image.new('RGBA', (64, 64))
    for y in range(64):
        for x in range(64):
            if rows[y][x]: im.putpixel((x, y), expand(palette[rows[y][x]]) + (255,))
    return im
