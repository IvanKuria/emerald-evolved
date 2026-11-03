// Genetic palette transforms (port of pokeemerald-real-genetics src/genetics_palette.c, colour maths unchanged,
// plus a heritable hue gene). Integer only.
#include "global.h"
#include "constants/rgb.h"
#include "palette.h"
#include "pokemon.h"
#include "genetics.h"
#include "genetics_palette.h"
#include "sprite.h"
#include "graphics.h"
#include "pokemon_storage_system.h"
#include "constants/daycare.h"

#define OUTLINE_Y            40
#define WHITE_Y              200
#define WHITE_CHROMA         48
#define ALBINO_LO            150
#define ALBINO_HI            248
#define ALBINO_HUE           160
#define ALBINO_CHROMA        26
#define ALBINO_RESIDUE_SHIFT 3
#define RED_HUE_BASE         0
#define RED_HUE_SPAN         80
#define RED_HUE_LUMA         120
#define RED_CHROMA_MIN       120
#define BLACK_LO             18
#define BLACK_HI             96
#define BLACK_CHROMA_NUM     1
#define BLACK_CHROMA_DEN     6
#define BLACK_CHROMA_MAX     16
#define BLACK_OUTLINE_NUM    1
#define BLACK_OUTLINE_DEN    3

enum { KIND_OUTLINE, KIND_WHITE, KIND_PIGMENT };

static inline s32 Expand5(s32 v)
{
    return (v << 3) | (v >> 2);
}

static s32 To5(s32 v)
{
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (v * 31 + 127) / 255;
}

static inline s32 Luma(s32 r, s32 g, s32 b)
{
    return (r * 77 + g * 150 + b * 29) >> 8;
}

static s32 Max3(s32 a, s32 b, s32 c)
{
    s32 m = a > b ? a : b;
    return m > c ? m : c;
}

static s32 Min3(s32 a, s32 b, s32 c)
{
    s32 m = a < b ? a : b;
    return m < c ? m : c;
}

static s32 Hue(s32 r, s32 g, s32 b)
{
    s32 mx = Max3(r, g, b);
    s32 c = mx - Min3(r, g, b);
    s32 h;

    if (c == 0)
        return 0;
    if (mx == r)
        h = (g >= b) ? (256 * (g - b)) / c : 1536 - (256 * (b - g)) / c;
    else if (mx == g)
        h = (b >= r) ? 512 + (256 * (b - r)) / c : 512 - (256 * (r - b)) / c;
    else
        h = (r >= g) ? 1024 + (256 * (r - g)) / c : 1024 - (256 * (g - r)) / c;
    if (h >= 1536)
        h -= 1536;
    return h;
}

static void PureHue(s32 h, s32 *p)
{
    s32 f = h & 255;

    switch (h >> 8)
    {
    case 0:  p[0] = 255;     p[1] = f;       p[2] = 0;       break;
    case 1:  p[0] = 255 - f; p[1] = 255;     p[2] = 0;       break;
    case 2:  p[0] = 0;       p[1] = 255;     p[2] = f;       break;
    case 3:  p[0] = 0;       p[1] = 255 - f; p[2] = 255;     break;
    case 4:  p[0] = f;       p[1] = 0;       p[2] = 255;     break;
    default: p[0] = 255;     p[1] = 0;       p[2] = 255 - f; break;
    }
}

// a colour with luma `y`, hue `h` and as much of `chroma` (0..255) as fits without clipping
static void FromLumaHueChroma(s32 y, s32 h, s32 chroma, s32 *out)
{
    s32 p[3];
    s32 yp, d, lim, v, i;

    PureHue(h, p);
    yp = Luma(p[0], p[1], p[2]);
    for (i = 0; i < 3; i++)
    {
        d = p[i] - yp;
        if (d > 0)
            lim = ((255 - y) * 255) / d;
        else if (d < 0)
            lim = (y * 255) / (-d);
        else
            continue;
        if (chroma > lim)
            chroma = lim;
    }
    for (i = 0; i < 3; i++)
    {
        d = p[i] - yp;
        if (d >= 0)
            v = y + (d * chroma) / 255;
        else
            v = y - ((-d) * chroma) / 255;
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        out[i] = v;
    }
}

static u8 Classify(s32 r, s32 g, s32 b, s32 *y, s32 *c)
{
    *y = Luma(r, g, b);
    *c = Max3(r, g, b) - Min3(r, g, b);
    if (*y < OUTLINE_Y)
        return KIND_OUTLINE;
    if (*y >= WHITE_Y && *c < WHITE_CHROMA)
        return KIND_WHITE;
    return KIND_PIGMENT;
}

#define CLUSTER_HALF_WIDTH 128   // +-30 degrees: the body's shading ramp, not the cheeks or eyes
#define CLUSTER_MIN_CHROMA 24

static s32 HueDist(s32 a, s32 b)
{
    s32 d = a > b ? a - b : b - a;
    return d > 768 ? 1536 - d : d;
}

// The hue gene recolours the dominant body-hue cluster only: the hue band (+-30 degrees) holding the most weight
// (pixel counts when known, else one per palette entry). Returns a 16-bit mask of the entries to rotate.
static u16 DominantHueCluster(const u16 *pal, const u16 *weights)
{
    s32 hue[16], w[16], i, j, best = -1, bestScore = 0, score, centre, sum, wsum;
    u16 mask = 0;
    for (i = 1; i < 16; i++)
    {
        s32 r = Expand5(GET_R(pal[i])), g = Expand5(GET_G(pal[i])), b = Expand5(GET_B(pal[i])), y, c;
        w[i] = 0;
        if (Classify(r, g, b, &y, &c) != KIND_PIGMENT || c < CLUSTER_MIN_CHROMA)
            continue;
        hue[i] = Hue(r, g, b);
        w[i] = weights ? weights[i] : 1;
    }
    for (i = 1; i < 16; i++)
    {
        if (w[i] == 0)
            continue;
        for (score = 0, j = 1; j < 16; j++)
            if (w[j] && HueDist(hue[i], hue[j]) <= CLUSTER_HALF_WIDTH)
                score += w[j];
        if (score > bestScore)
            bestScore = score, best = i;
    }
    if (best < 0)
        return 0;
    // re-centre on the weighted circular mean of that band, then take everything within it
    centre = hue[best];
    for (sum = 0, wsum = 0, j = 1; j < 16; j++)
    {
        if (w[j] && HueDist(centre, hue[j]) <= CLUSTER_HALF_WIDTH)
        {
            s32 dh = hue[j] - centre;
            if (dh > 768) dh -= 1536;
            if (dh < -768) dh += 1536;
            sum += dh * w[j]; wsum += w[j];
        }
    }
    centre += sum / wsum;
    if (centre < 0) centre += 1536;
    if (centre >= 1536) centre -= 1536;
    for (i = 1; i < 16; i++)
    {
        s32 r = Expand5(GET_R(pal[i])), g = Expand5(GET_G(pal[i])), b = Expand5(GET_B(pal[i])), y, c;
        if (Classify(r, g, b, &y, &c) == KIND_PIGMENT && c > 0 && HueDist(centre, Hue(r, g, b)) <= CLUSTER_HALF_WIDTH)
            mask |= 1 << i;
    }
    return mask;
}

void ApplyGenesTo16(u16 *pal, u8 coat, s32 hueShift)
{
    ApplyGenesTo16Weighted(pal, coat, hueShift, NULL);
}

void ApplyGenesTo16Weighted(u16 *pal, u8 coat, s32 hueShift, const u16 *weights)
{
    s32 lo = 255, hi = 0, span, t, y, c, r, g, b, d, i, k;
    s32 out[3], rgb[3];
    u8 kind;
    u16 hueMask = (hueShift != 0 && coat != COAT_ALBINO) ? DominantHueCluster(pal, weights) : 0;

    for (i = 1; i < 16; i++)
    {
        r = Expand5(GET_R(pal[i]));
        g = Expand5(GET_G(pal[i]));
        b = Expand5(GET_B(pal[i]));
        if (Classify(r, g, b, &y, &c) == KIND_PIGMENT)
        {
            if (y < lo)
                lo = y;
            if (y > hi)
                hi = y;
        }
    }
    span = (hi > lo) ? hi - lo : 0;

    for (i = 1; i < 16; i++)
    {
        rgb[0] = r = Expand5(GET_R(pal[i]));
        rgb[1] = g = Expand5(GET_G(pal[i]));
        rgb[2] = b = Expand5(GET_B(pal[i]));
        kind = Classify(r, g, b, &y, &c);
        if (kind == KIND_WHITE)
            continue;
        if (span)
            t = (kind == KIND_PIGMENT) ? ((y - lo) * 255) / span : 0;
        else
            t = y;

        switch (coat)
        {
        case COAT_ALBINO:
            if (kind == KIND_OUTLINE)
                continue;
            FromLumaHueChroma(ALBINO_LO + (t * (ALBINO_HI - ALBINO_LO)) / 255, ALBINO_HUE, ALBINO_CHROMA, out);
            for (k = 0; k < 3; k++)
            {
                d = rgb[k] - y;
                out[k] += (d >= 0) ? (d >> ALBINO_RESIDUE_SHIFT) : -((-d) >> ALBINO_RESIDUE_SHIFT);
            }
            break;
        case COAT_RED:
            if (kind == KIND_OUTLINE)
                continue;
            FromLumaHueChroma(y,
                              RED_HUE_BASE + (Hue(r, g, b) * RED_HUE_SPAN) / 1536 + (y * RED_HUE_LUMA) / 255,
                              c > RED_CHROMA_MIN ? c : RED_CHROMA_MIN,
                              out);
            break;
        case COAT_BLACK:
            if (kind == KIND_OUTLINE)
            {
                out[0] = out[1] = out[2] = (y * BLACK_OUTLINE_NUM) / BLACK_OUTLINE_DEN;
            }
            else
            {
                c = (c * BLACK_CHROMA_NUM) / BLACK_CHROMA_DEN;
                if (c > BLACK_CHROMA_MAX)
                    c = BLACK_CHROMA_MAX;
                FromLumaHueChroma(BLACK_LO + (t * (BLACK_HI - BLACK_LO)) / 255, Hue(r, g, b), c, out);
            }
            break;
        default:
            if (kind != KIND_PIGMENT || !(hueMask & (1 << i)))
                continue;
            out[0] = r; out[1] = g; out[2] = b;
            break;
        }
        // hue gene: rotate the pigment's hue, keep its luma and chroma, so shading survives
        if (kind == KIND_PIGMENT && (hueMask & (1 << i)))
        {
            s32 h2 = Hue(out[0], out[1], out[2]) + hueShift;
            s32 c2 = Max3(out[0], out[1], out[2]) - Min3(out[0], out[1], out[2]);
            s32 y2 = Luma(out[0], out[1], out[2]);
            while (h2 < 0) h2 += 1536;
            while (h2 >= 1536) h2 -= 1536;
            FromLumaHueChroma(y2, h2, c2, out);
        }
        pal[i] = RGB(To5(out[0]), To5(out[1]), To5(out[2]));
    }
}


static EWRAM_DATA u16 sGenePal[8][16] = {0};
static EWRAM_DATA u8 sGenePalSlot = 0;

const u16 *GeneticMonPalette(const struct BoxPokemon *boxMon, const u16 *base)
{
    struct Genome g;
    u8 coat;
    s32 hue;
    u16 *dst;

    if (base == NULL || !boxMon->hasSpecies || boxMon->isEgg)
        return base;
    GetBoxMonGenome(boxMon, &g);
    coat = GenomeCoat(&g);
    hue = GenomeHueShift(&g);
    if (coat == COAT_NORMAL && hue == 0)
        return base;
    dst = sGenePal[sGenePalSlot];
    sGenePalSlot = (sGenePalSlot + 1) & 7;
    CpuCopy16(base, dst, sizeof(sGenePal[0]));
    ApplyGenesTo16(dst, coat, hue);
    return dst;
}

// ---------------------------------------------------------------- party / PC-box / any mon icon
// Mon icons share 6 palettes, so a recoloured icon gets its own OBJ palette from a small pool keyed by
// (species, icon palette, coat, hue). Pool entries are reclaimed when no sprite uses them, and the pool never takes the
// last GENE_ICON_RESERVE free OBJ palettes, so screens that load palettes later still find room. If there is no room
// the icon keeps its species colours.
#define GENE_ICON_TAG      0xE7A0
#define GENE_ICON_SLOTS    8
#define GENE_ICON_RESERVE  1

struct GeneIconKey { u16 species; u8 basePal; u8 coat; s16 hue; };
static EWRAM_DATA struct GeneIconKey sGeneIconKeys[GENE_ICON_SLOTS] = {0};
static EWRAM_DATA const struct BoxPokemon *sIconHint = NULL;

void SetMonIconGenomeHint(const struct BoxPokemon *boxMon) { sIconHint = boxMon; }

// species 0 = match the personality alone (an evolving or transformed Pokémon is drawn as another species)
static bool32 IsMatch(const struct BoxPokemon *b, u32 personality, u16 species)
{
    return b->hasSpecies && b->personality == personality
        && (species == SPECIES_NONE || GetBoxMonData((struct BoxPokemon *)b, MON_DATA_SPECIES) == species);
}

// Icons and many screens draw a Pokémon from species + personality; find the Pokémon that is meant.
static const struct BoxPokemon *FindOwnedBoxMon(u16 species, u32 personality)
{
    u32 i, j;
    if (personality == 0)
        return NULL;
    if (sIconHint != NULL && IsMatch(sIconHint, personality, species))
        return sIconHint;
    for (i = 0; i < PARTY_SIZE; i++)
        if (IsMatch(&gParties[B_TRAINER_PLAYER][i].box, personality, species))
            return &gParties[B_TRAINER_PLAYER][i].box;
    for (i = 0; i < TOTAL_BOXES_COUNT; i++)
        for (j = 0; j < IN_BOX_COUNT; j++)
            if (IsMatch(&gPokemonStoragePtr->boxes[i][j], personality, species))
                return &gPokemonStoragePtr->boxes[i][j];
    for (i = 0; i < DAYCARE_MON_COUNT; i++)
        if (IsMatch(&gSaveBlock1Ptr->daycare.mons[i].mon, personality, species))
            return &gSaveBlock1Ptr->daycare.mons[i].mon;
    for (i = 0; i < PARTY_SIZE; i++)
        if (IsMatch(&gParties[B_TRAINER_OPPONENT_A][i].box, personality, species))
            return &gParties[B_TRAINER_OPPONENT_A][i].box;
    return NULL;
}

const u16 *GeneticPaletteForPersonality(u32 personality, const u16 *base)
{
    const struct BoxPokemon *boxMon = FindOwnedBoxMon(SPECIES_NONE, personality);
    return boxMon ? GeneticMonPalette(boxMon, base) : base;
}

static bool32 IsPaletteNumInUse(u32 paletteNum)
{
    u32 i;
    for (i = 0; i < MAX_SPRITES; i++)
        if (gSprites[i].inUse && gSprites[i].oam.paletteNum == paletteNum)
            return TRUE;
    return FALSE;
}

void SetMonIconGenePalette(u8 spriteId, u16 species, u32 personality, u8 basePal, const u8 *iconTiles)
{
    const struct BoxPokemon *boxMon;
    struct Genome g;
    struct GeneIconKey key;
    u16 pal[16], weights[16];
    u32 k, free = GENE_ICON_SLOTS, index;

    if (spriteId >= MAX_SPRITES || (boxMon = FindOwnedBoxMon(species, personality)) == NULL || boxMon->isEgg)
        return;
    sIconHint = NULL;
    GetBoxMonGenome(boxMon, &g);
    key.species = species; key.basePal = basePal; key.coat = GenomeCoat(&g);
    key.hue = ((GenomeHueShift(&g) + (GenomeHueShift(&g) >= 0 ? 48 : -48)) / 96) * 96;   // 22.5-degree buckets at icon size
    if (key.coat == COAT_NORMAL && key.hue == 0)
        return;
    for (k = 0; k < GENE_ICON_SLOTS; k++)
    {
        index = IndexOfSpritePaletteTag(GENE_ICON_TAG + k);
        if (index == 0xFF)
        {
            if (free == GENE_ICON_SLOTS)
                free = k;
            continue;
        }
        if (memcmp(&sGeneIconKeys[k], &key, sizeof(key)) == 0)
        {
            gSprites[spriteId].oam.paletteNum = index;
            return;
        }
    }
    if (free == GENE_ICON_SLOTS)    // pool full: reclaim an entry no sprite uses
    {
        for (k = 0; k < GENE_ICON_SLOTS; k++)
        {
            if (!IsPaletteNumInUse(IndexOfSpritePaletteTag(GENE_ICON_TAG + k)))
            {
                FreeSpritePaletteByTag(GENE_ICON_TAG + k);
                free = k;
                break;
            }
        }
        if (free == GENE_ICON_SLOTS)
            return;
    }
    if (CountFreeSpritePalettes() <= GENE_ICON_RESERVE)
        return;
    // weight each palette entry by how many pixels of the icon's first frame use it
    memset(weights, 0, sizeof(weights));
    for (k = 0; k < 32 * 32 / 2; k++)
    {
        weights[iconTiles[k] & 0xF]++;
        weights[iconTiles[k] >> 4]++;
    }
    CpuCopy16(gMonIconPalettes[basePal], pal, sizeof(pal));
    ApplyGenesTo16Weighted(pal, key.coat, key.hue, weights);
    {
        struct SpritePalette sp = {pal, GENE_ICON_TAG + free};
        index = LoadSpritePalette(&sp);
    }
    if (index == 0xFF)
        return;
    sGeneIconKeys[free] = key;
    gSprites[spriteId].oam.paletteNum = index;
}
