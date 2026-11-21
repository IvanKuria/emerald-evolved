"""Cross-breed hybrid sprites: the table the ROM is built from (GAME_SPEC feature 3).

CANONICAL source: the owner-picked recipe in
  deathnote-lifespan/.claude/worktrees/pokemon-crossbreed-thumb/research/pokemon-crossbreed/sprites/modern_hybrids.py
(reference PNGs: deathnote-lifespan/video/public/mc-chem-v2/crossbreed/sprites_modern/). Every number below is copied
from it; test/hybrid/run.sh diffs the ROM's in-game render against those PNGs pixel for pixel.

A hybrid is drawn as: the BODY species' real indexed sprite (the species being drawn, i.e. the mother's line) with its body
ramp replaced by the COLOUR species' (father's) colours, plus real body parts cut by palette index from each PART
species' front sprite, placed per PLACEMENTS, then merged to the 15 colours a GBA sprite palette holds
(hybrid_lib.merge15 == src/hybrid_sprite.c MergeColours). Art: the ROM's own modern sprites (owner decision B).
Coordinates are in the body sprite's own 64x64 frame.
"""

# Owner-picked ramp maps (modern_hybrids.py RAMPS): body palette index -> colour species palette index.
RAMP_MAPS = {
    ('charmander', 'bulbasaur'): {6: 2, 7: 3, 8: 4, 9: 5},
    ('charmander', 'pikachu'): {6: 2, 7: 3, 8: 4, 9: 6},
    ('pikachu', 'charmander'): {2: 6, 3: 7, 4: 8, 5: 9, 6: 9},
}

# Body ramps (light -> dark) for pairs with no owner map: body ramp role i takes colour ramp role min(i, len - 1),
# the same rule the owner's pika_in_char map follows. Species in RAMP_SPECIES but not here get bake.py's auto ramp.
RAMPS = {
    'charmander': [6, 7, 8, 9],
    'bulbasaur': [2, 3, 4, 5],
    'pikachu': [2, 3, 4, 5, 6],
}
RAMP_SPECIES = [
    'bulbasaur', 'ivysaur', 'venusaur', 'charmander', 'charmeleon', 'charizard', 'squirtle', 'wartortle', 'blastoise',
    'chikorita', 'bayleef', 'meganium', 'cyndaquil', 'quilava', 'typhlosion', 'totodile', 'croconaw', 'feraligatr',
    'treecko', 'grovyle', 'sceptile', 'torchic', 'combusken', 'blaziken', 'mudkip', 'marshtomp', 'swampert',
    'pichu', 'pikachu', 'raichu', 'eevee',
    # crossbreed (xg-core): the Route 117 wild-hybrid pairs (Illumise x Volbeat, Poochyena x Seedot lines)
    'volbeat', 'illumise', 'poochyena', 'mightyena', 'seedot', 'nuzleaf', 'shiftry',
]


def cut(idx, box):
    """a real part from the part species' front frame 1: pixels inside box (x0, y0, x1, y1 inclusive) whose palette
    index is in idx, cropped to its pixels (modern_hybrids.part)"""
    return dict(idx=frozenset(idx), box=box)


BULB = cut({10, 11, 12, 13, 14}, (26, 18, 48, 35))        # bulb option 3
EAR_L = cut({1, 4, 5, 7, 13, 14}, (16, 9, 22, 17))         # ears option 1
EAR_R = cut({1, 4, 5, 6, 13, 14}, (34, 14, 45, 18))
FLAME = cut({9, 13, 14, 15}, (45, 24, 53, 38))             # flame option 2

# Body regions a part rides with into anim frame 2 (modern_hybrids.shift: best exact-index match, +-6 px).
CH_BODY, CH_HEAD, PI_TAIL = (22, 30, 40, 46), (18, 12, 32, 27), (44, 20, 58, 30)


def piece(src, angle, mode, at, region, behind):
    """angle: PIL rotate degrees (NEAREST, expand; negative = clockwise). mode 'topleft': the rotated image's top-left
    at `at`; 'root': the middle of the rotated part's bottom edge (bbox) at `at`. region: the body region whose frame-2
    shift moves the piece. behind: drawn behind the body (else in front)."""
    return dict(src=src, angle=angle, mode=mode, at=at, region=region, behind=behind)


# PLACEMENTS[(body, part species)] = pieces in draw order. Pairs not listed draw no part (colour only).
# Back sprites have no owner placement yet: they get the colours only.
PLACEMENTS = {
    ('charmander', 'bulbasaur'): [piece(BULB, -15, 'topleft', (22, 23), CH_BODY, True)],
    ('charmander', 'pikachu'): [piece(EAR_R, 0, 'root', (29, 15), CH_HEAD, True),
                                piece(EAR_L, 0, 'root', (24, 14), CH_HEAD, True)],
    ('pikachu', 'charmander'): [piece(FLAME, -20, 'root', (53, 23), PI_TAIL, False)],
}

# The hybrids the test renders and diffs: (reference name, body, colour species, part species in draw order).
CASES = [
    ('hybrid_pika_x_char', 'pikachu', 'charmander', ['charmander']),
    ('hybrid_char_x_pika', 'charmander', 'pikachu', []),
    ('hybrid_char_x_bulb', 'charmander', 'bulbasaur', ['bulbasaur']),
    ('hybrid_three', 'charmander', 'pikachu', ['bulbasaur', 'pikachu']),
]
