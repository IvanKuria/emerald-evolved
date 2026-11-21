// Cross-breed hybrid sprites (GAME_SPEC feature 3): runtime compositing. A step-for-step port of
// tools/hybrid/hybrid_lib.compose(), which is itself a port of the owner's modern_hybrids.py; test/hybrid/run.sh diffs the
// in-game result against the owner's reference PNGs.
//
// Pipeline for one hybrid (body = the species being drawn):
//   1. virtual colours: the body's 16 palette colours with the colour species' ramp map applied, then every part colour
//      (16 + k), exact duplicates shared;
//   2. each view is composed into virtual slots: behind layers, then the body's non-zero pixels, then front layers;
//   3. merge15 over front frame 1 + 2 by colour value (RGB555): while more than 15 colours, the closest pair (squared
//      distance; ties -> lowest min((count, colour)), then first pair in colour order) is merged into the one with the
//      larger (count, colour);
//   4. surviving colours, in colour order, become palette indices 1..n. Back-view colours the front never used go to
//      the nearest surviving colour. The pic only depends on the normal colours, so the shiny palette is the same
//      slots filled from the shiny base.
#include "global.h"
#include "hybrid_sprite.h"
#include "decompress.h"
#include "genetics_palette.h"
#include "malloc.h"
#include "pokemon.h"
#include "constants/species.h"

#include "data/hybrid_sprites.h"

#define V_SIZE   (16 + HYBRID_MAX_PART_COLOURS)
#define PIC_W    64
#define FRAME_PX (64 * 64)

struct HybridState
{
    bool8 valid;
    u16 species;
    u32 personality;
    struct HybridLook look;
    u8 mapCount;
    u8 mapBody[16];
    u16 mapColour[16];              // RGB555 the ramp map puts in mapBody[i]
    u8 partCount;
    u16 vNormal[V_SIZE];            // virtual colours from the normal base palette
    u8 finalIdx[V_SIZE];            // virtual slot -> palette index
    u8 finalCount;
    u8 palSrc[16];                  // palette index -> virtual slot it takes its colour from
};

static EWRAM_DATA struct HybridState sHybrid = {0};
static EWRAM_DATA u16 sHybridPal[8][16] = {0};
static EWRAM_DATA u8 sHybridPalSlot = 0;
static EWRAM_DATA bool8 sInHybrid = FALSE;

#if EVO_TEST
#define TEST_LOOKS 8
static EWRAM_DATA u32 sTestPersonality[TEST_LOOKS] = {0};
static EWRAM_DATA struct HybridLook sTestLook[TEST_LOOKS] = {0};

void HybridSetTestLook(u32 personality, const struct HybridLook *look)
{
    u32 i;
    sHybrid.valid = FALSE;
    if (look == NULL)
    {
        memset(sTestPersonality, 0, sizeof(sTestPersonality));
        return;
    }
    for (i = 0; i < TEST_LOOKS; i++)
    {
        if (sTestPersonality[i] == 0 || sTestPersonality[i] == personality)
        {
            sTestPersonality[i] = personality;
            sTestLook[i] = *look;
            return;
        }
    }
}
#endif

bool32 GetBoxMonHybridLook(const struct BoxPokemon *boxMon, struct HybridLook *look)
{
    memset(look, 0, sizeof(*look));
    if (boxMon == NULL || !boxMon->hasSpecies || boxMon->isEgg)
        return FALSE;
#if EVO_TEST
    {
        u32 i;
        for (i = 0; i < TEST_LOOKS; i++)
        {
            if (sTestPersonality[i] != 0 && sTestPersonality[i] == boxMon->personality)
            {
                *look = sTestLook[i];
                return TRUE;
            }
        }
    }
#endif
    // TODO(feature 2, cross-species eggs): read the father / part species from the genome's lineage once it is stored.
    return FALSE;
}

// ---------------------------------------------------------------- small helpers
static inline u32 Red(u16 c)   { return c & 31; }
static inline u32 Green(u16 c) { return (c >> 5) & 31; }
static inline u32 Blue(u16 c)  { return (c >> 10) & 31; }
static inline u32 ColourKey(u16 c) { return (Red(c) << 10) | (Green(c) << 5) | Blue(c); }   // Python (r, g, b) order

static u32 Dist(u16 a, u16 b)
{
    s32 dr = Red(a) - Red(b), dg = Green(a) - Green(b), db = Blue(a) - Blue(b);
    return dr * dr + dg * dg + db * db;
}

static inline u32 GetPx(const u8 *pic, u32 x, u32 y)
{
    u8 b = pic[((y >> 3) * (PIC_W / 8) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    return (x & 1) ? (b >> 4) : (b & 0xF);
}

static inline void SetPx(u8 *pic, u32 x, u32 y, u32 v)
{
    u8 *b = &pic[((y >> 3) * (PIC_W / 8) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    *b = (x & 1) ? ((*b & 0x0F) | (v << 4)) : ((*b & 0xF0) | v);
}

static const struct HybridRamp *FindRamp(u16 species)
{
    u32 i;
    for (i = 0; i < ARRAY_COUNT(sHybridRamps); i++)
        if (sHybridRamps[i].species == species)
            return &sHybridRamps[i];
    return NULL;
}

// = hybrid_lib.ramp_map: the owner's map, else body ramp role i -> colour ramp role min(i, len - 1)
static void BuildRampMap(struct HybridState *s, u16 body, u16 colourSpecies)
{
    const u16 *cpal;
    const struct HybridRamp *br, *cr;
    u32 i;

    s->mapCount = 0;
    if (colourSpecies == SPECIES_NONE || colourSpecies == body)
        return;
    cpal = GetMonSpritePalFromSpeciesIsEgg(colourSpecies, FALSE, FALSE, FALSE);
    for (i = 0; i < ARRAY_COUNT(sHybridRampMaps); i++)
    {
        const struct HybridRampMap *m = &sHybridRampMaps[i];
        if (m->bodySpecies == body && m->colourSpecies == colourSpecies)
        {
            u32 k;
            for (k = 0; k < m->count; k++)
            {
                s->mapBody[k] = m->bodyIdx[k];
                s->mapColour[k] = cpal[m->colourIdx[k]];
            }
            s->mapCount = m->count;
            return;
        }
    }
    br = FindRamp(body);
    cr = FindRamp(colourSpecies);
    if (br == NULL || cr == NULL)
        return;
    for (i = 0; i < br->count; i++)
    {
        s->mapBody[i] = br->idx[i];
        s->mapColour[i] = cpal[cr->idx[min(i, (u32)cr->count - 1)]];
    }
    s->mapCount = br->count;
}

static void VirtualFromBase(const struct HybridState *s, const u16 *base, u16 *v)
{
    u32 i;
    for (i = 0; i < 16; i++)
        v[i] = base[i];
    for (i = 0; i < s->mapCount; i++)
        v[s->mapBody[i]] = s->mapColour[i];
}

// part colours: one list for the whole look, in look-part order then layer order, exact duplicates shared
static u32 PartSlot(struct HybridState *s, u16 colour, bool32 add)
{
    u32 k;
    for (k = 0; k < s->partCount; k++)
        if (s->vNormal[16 + k] == colour)
            return 16 + k;
    if (!add || s->partCount == HYBRID_MAX_PART_COLOURS)
        return 0;
    s->vNormal[16 + s->partCount] = colour;
    return 16 + s->partCount++;
}

static void DrawLayers(struct HybridState *s, u8 *vbuf, u32 view, bool32 behind)
{
    u32 p, i, x, y;
    for (p = 0; p < HYBRID_MAX_PARTS; p++)
    {
        if (s->look.partSpecies[p] == SPECIES_NONE)
            continue;
        for (i = 0; i < ARRAY_COUNT(sHybridLayers); i++)
        {
            const struct HybridLayer *l = &sHybridLayers[i];
            if (l->bodySpecies != s->species || l->partSpecies != s->look.partSpecies[p] || l->view != view || l->behind != behind)
                continue;
            for (y = 0; y < l->h; y++)
            {
                for (x = 0; x < l->w; x++)
                {
                    u32 k = l->pixels[y * l->w + x];
                    if (k != 0)
                        vbuf[(l->y + y) * 64 + l->x + x] = PartSlot(s, l->colours[k - 1], FALSE);
                }
            }
        }
    }
}

// one view of the body pic (frame `frame` of a front pic, or the back pic) composed into virtual slots
static void ComposeView(struct HybridState *s, const u8 *pic, u32 frame, u32 view, u8 *vbuf)
{
    u32 x, y, v;
    memset(vbuf, 0, FRAME_PX);
    DrawLayers(s, vbuf, view, TRUE);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if ((v = GetPx(pic, x, y + frame * 64)) != 0)
                vbuf[y * 64 + x] = v;
    DrawLayers(s, vbuf, view, FALSE);
}

// ---------------------------------------------------------------- merge15
static bool32 TupleLess(u32 countA, u16 a, u32 countB, u16 b)
{
    return countA != countB ? countA < countB : ColourKey(a) < ColourKey(b);
}

static void Merge(struct HybridState *s, const u32 *slotCount)
{
    u16 col[V_SIZE], rep[V_SIZE];       // distinct colours (sorted by key) and what each became
    u32 cnt[V_SIZE];
    u8 live[V_SIZE];
    u32 n = 0, nLive, i, j, k;

    // distinct colours used by the front frames, with pixel counts
    for (i = 1; i < 16 + s->partCount; i++)
    {
        if (slotCount[i] == 0)
            continue;
        for (k = 0; k < n && col[k] != s->vNormal[i]; k++)
            ;
        if (k == n)
        {
            col[n] = s->vNormal[i];
            cnt[n++] = 0;
        }
        cnt[k] += slotCount[i];
    }
    // sort by colour key (insertion sort, n <= 48)
    for (i = 1; i < n; i++)
    {
        for (j = i; j > 0 && ColourKey(col[j - 1]) > ColourKey(col[j]); j--)
        {
            u16 tc = col[j]; u32 tn = cnt[j];
            col[j] = col[j - 1]; cnt[j] = cnt[j - 1];
            col[j - 1] = tc; cnt[j - 1] = tn;
        }
    }
    for (i = 0; i < n; i++)
    {
        rep[i] = i;
        live[i] = i;
    }
    nLive = n;
    while (nLive > 15)
    {
        u32 bestD = 0xFFFFFFFF, bestMinCount = 0, bestA = 0, bestB = 0;
        u16 bestMinCol = 0;
        for (i = 0; i < nLive; i++)
        {
            for (j = i + 1; j < nLive; j++)
            {
                u32 a = live[i], b = live[j], d = Dist(col[a], col[b]);
                u32 mc; u16 mcol;
                if (TupleLess(cnt[a], col[a], cnt[b], col[b])) { mc = cnt[a]; mcol = col[a]; }
                else { mc = cnt[b]; mcol = col[b]; }
                if (d < bestD || (d == bestD && TupleLess(mc, mcol, bestMinCount, bestMinCol)))
                {
                    bestD = d; bestMinCount = mc; bestMinCol = mcol; bestA = a; bestB = b;
                }
            }
        }
        {
            u32 keep, drop;
            if (TupleLess(cnt[bestA], col[bestA], cnt[bestB], col[bestB])) { keep = bestB; drop = bestA; }
            else { keep = bestA; drop = bestB; }
            cnt[keep] += cnt[drop];
            cnt[drop] = 0;
            for (k = 0; k < n; k++)
                if (rep[k] == drop)
                    rep[k] = keep;
            for (k = 0; k < nLive && live[k] != drop; k++)
                ;
            for (; k + 1 < nLive; k++)
                live[k] = live[k + 1];
            nLive--;
        }
    }

    // live[] stays in colour order: palette index i + 1 = live[i]
    s->finalCount = nLive;
    s->finalIdx[0] = 0;
    for (i = 1; i < V_SIZE; i++)
    {
        u16 c = s->vNormal[i];
        u32 target = 0xFF;
        if (i >= 16 + s->partCount)
        {
            s->finalIdx[i] = 0;
            continue;
        }
        for (k = 0; k < n; k++)
        {
            if (col[k] == c)
            {
                target = rep[k];
                break;
            }
        }
        if (target == 0xFF)     // a colour the front never uses (back view only): nearest surviving colour
        {
            u32 bestD = 0xFFFFFFFF;
            for (k = 0; k < nLive; k++)
            {
                u32 d = Dist(col[live[k]], c);
                if (d < bestD) { bestD = d; target = live[k]; }   // live[] is in colour order: ties keep the lowest
            }
        }
        for (k = 0; k < nLive && live[k] != target; k++)
            ;
        s->finalIdx[i] = k + 1;
    }
    // each palette entry takes its colour from the lowest virtual slot with that (normal) colour
    for (k = 0; k < nLive; k++)
    {
        for (i = 1; i < 16 + s->partCount && s->vNormal[i] != col[live[k]]; i++)
            ;
        s->palSrc[k + 1] = i;
    }
}

// ---------------------------------------------------------------- state
static bool32 BuildState(u16 species, u32 personality, const struct HybridLook *look)
{
    struct HybridState *s = &sHybrid;
    u32 slotCount[V_SIZE];
    u8 *pic, *vbuf;
    u32 i, p, frame;
    bool32 hasLayers = FALSE;

    if (s->valid && s->species == species && s->personality == personality && memcmp(&s->look, look, sizeof(*look)) == 0)
        return TRUE;
    s->valid = FALSE;
    s->species = species;
    s->personality = personality;
    s->look = *look;
    BuildRampMap(s, species, look->colourSpecies);
    VirtualFromBase(s, GetMonSpritePalFromSpeciesIsEgg(species, FALSE, IsPersonalityFemale(species, personality), FALSE), s->vNormal);
    s->partCount = 0;
    for (p = 0; p < HYBRID_MAX_PARTS; p++)
    {
        for (i = 0; i < ARRAY_COUNT(sHybridLayers); i++)
        {
            const struct HybridLayer *l = &sHybridLayers[i];
            u32 k;
            if (look->partSpecies[p] == SPECIES_NONE || l->bodySpecies != species || l->partSpecies != look->partSpecies[p])
                continue;
            hasLayers = TRUE;
            for (k = 0; k < l->colourCount; k++)
                PartSlot(s, l->colours[k], TRUE);
        }
    }
    if (s->mapCount == 0 && !hasLayers)
        return FALSE;               // this look changes nothing: draw the plain species

    pic = Alloc(FRAME_PX);          // 64x128 4bpp front pic = 4096 bytes
    vbuf = Alloc(FRAME_PX);
    if (pic == NULL || vbuf == NULL)
    {
        TRY_FREE_AND_SET_NULL(pic);
        TRY_FREE_AND_SET_NULL(vbuf);
        return FALSE;
    }
    sInHybrid = TRUE;
    LoadSpecialPokePicIsEgg(pic, species, personality, TRUE, FALSE);
    sInHybrid = FALSE;
    memset(slotCount, 0, sizeof(slotCount));
    for (frame = 0; frame < 2; frame++)
    {
        ComposeView(s, pic, frame, frame == 0 ? HYBRID_VIEW_FRONT_1 : HYBRID_VIEW_FRONT_2, vbuf);
        for (i = 0; i < FRAME_PX; i++)
            slotCount[vbuf[i]]++;
    }
    Free(pic);
    Free(vbuf);
    slotCount[0] = 0;
    Merge(s, slotCount);
    s->valid = TRUE;
    return TRUE;
}

static bool32 StateFor(const struct BoxPokemon *boxMon, u16 species, u32 personality)
{
    struct HybridLook look;
    if (sInHybrid || boxMon == NULL || !GetBoxMonHybridLook(boxMon, &look))
        return FALSE;
    return BuildState(species, personality, &look);
}

// ---------------------------------------------------------------- hooks
void HybridApplyPic(void *dest, u16 species, u32 personality, bool32 isFrontPic)
{
    u8 *pic = dest, *vbuf;
    u32 frame, frames = isFrontPic ? 2 : 1, i;

    if (sInHybrid || !StateFor(FindOwnedBoxMonByPersonality(personality), species, personality))
        return;
    vbuf = Alloc(FRAME_PX);
    if (vbuf == NULL)
        return;
    for (frame = 0; frame < frames; frame++)
    {
        u32 view = !isFrontPic ? HYBRID_VIEW_BACK : (frame == 0 ? HYBRID_VIEW_FRONT_1 : HYBRID_VIEW_FRONT_2);
        ComposeView(&sHybrid, pic, frame, view, vbuf);
        for (i = 0; i < FRAME_PX; i++)
            SetPx(pic, i & 63, (i >> 6) + frame * 64, sHybrid.finalIdx[vbuf[i]]);
    }
    Free(vbuf);
}

const u16 *HybridBoxMonPalette(const struct BoxPokemon *boxMon, u16 species, bool32 isShiny, const u16 *base)
{
    u16 v[V_SIZE], *dst;
    u32 k;

    if (base == NULL || !StateFor(boxMon, species, boxMon->personality))
        return base;
    VirtualFromBase(&sHybrid, base, v);
    for (k = 16; k < V_SIZE; k++)
        v[k] = sHybrid.vNormal[k];
    dst = sHybridPal[sHybridPalSlot];
    sHybridPalSlot = (sHybridPalSlot + 1) & 7;
    dst[0] = base[0];
    for (k = 1; k < 16; k++)
        dst[k] = (k <= sHybrid.finalCount) ? v[sHybrid.palSrc[k]] : 0;
    (void)isShiny;   // the shiny base is already `base`: the pic does not depend on it
    return dst;
}

const u16 *HybridPalette(u16 species, u32 personality, bool32 isShiny, const u16 *base)
{
    const struct BoxPokemon *boxMon = FindOwnedBoxMonByPersonality(personality);
    return boxMon ? HybridBoxMonPalette(boxMon, species, isShiny, base) : base;
}
