// Real genetics, evolution edition. Pure logic (only Random()); packing into BoxPokemon lives in pokemon.c.
#include "global.h"
#include "genetics.h"
#include "random.h"

#define COAT_ALLELE_Q32     30370005u   // 1/sqrt(20000): each coat colour 1 in 20,000 wild
#define SHINY_ALLELE_Q32    262144u     // 1/16384: dominant, so wild shiny ~ 2q = 1 in 8,192
#define LETHAL_LOAD_P0      2404745173u // Poisson(0.58) P(0)
// Mutation, per gamete (DESIGN, tuned for play): 1 in N
#define MUT_SHINY_N         16384       // a brand-new shiny allele
#define MUT_HUE_N           24          // hue allele steps +-1
#define MUT_SPEC_N          64          // speciation loci: each transmitted ancestral copy becomes derived, 1 in 64
#define WILD_SPEC_A_N       20          // wild: each A-locus copy is derived 1 in 20; B copies are always ancestral
#define HUE_STEP            48          // hue units per allele step (11.25 degrees)

#define IV_COPIES (GENOME_IV_LOCI * 2)

// Canonical bit order (77 bits). Mapped onto BoxPokemon fields by ReadGenomeBits/WriteGenomeBits in pokemon.c.
#define B_IV      0    // 36: stat s, locus j at 2*(3s+j); bit 0 mother copy, bit 1 father copy
#define B_SHINY   36   // 2
#define B_COAT    38   // 6: C, E, A
#define B_LETHAL  44   // 7: carried lethal id, 0 = none
#define B_DEAD    51   // 1: inviable egg
#define B_HUE     52   // 8: mother allele (4-bit two's complement), then father allele
#define B_SPEC    60   // 16: locus i (A1..A4, B1..B4) at 60 + 2i
#define B_HAS     76   // 1

struct BitPool { u32 bits; u8 count; };

static u32 TakeRandomBit(struct BitPool *pool)
{
    u32 bit;
    if (pool->count == 0) { pool->bits = Random(); pool->count = 16; }
    bit = pool->bits & 1;
    pool->bits >>= 1;
    pool->count--;
    return bit;
}

static u32 NextSeeded(u32 *seed) { *seed = ISO_RANDOMIZE1(*seed); return *seed >> 16; }
static u8 RandomCopy(u32 q32) { return ((u32)Random32() < q32) ? 1 : 0; }
static u8 RandomDiploid(u32 q32) { return RandomCopy(q32) | (RandomCopy(q32) << 1); }
static bool8 OneIn(u32 n) { return (Random32() % n) == 0; }

static u32 CountStatCopies(const struct Genome *g, u8 stat)
{
    u32 i, c = 0;
    for (i = 0; i < GENOME_IV_LOCI; i++)
        c += (g->iv[stat][i] & 1) + ((g->iv[stat][i] >> 1) & 1);
    return c;
}

static s8 WildHueAllele(void)
{
    u32 r = Random() & 31;          // 30/32 wild type, 1/32 each +1 / -1
    return r == 0 ? -1 : (r == 1 ? 1 : 0);
}

void GenerateWildGenome(struct Genome *g)
{
    struct BitPool pool = {0};
    u32 i, j;

    memset(g, 0, sizeof(*g));
    for (i = 0; i < NUM_STATS; i++)
        for (j = 0; j < GENOME_IV_LOCI; j++)
            g->iv[i][j] = TakeRandomBit(&pool) | (TakeRandomBit(&pool) << 1);
    for (i = 0; i < NUM_COAT_LOCI; i++)
        g->coat[i] = RandomDiploid(COAT_ALLELE_Q32);
    g->shiny = RandomDiploid(SHINY_ALLELE_Q32);
    if ((u32)Random32() >= LETHAL_LOAD_P0)
        g->lethal[0] = (Random() % GENOME_LETHAL_LOCI) + 1;
    g->hue[0] = WildHueAllele();
    g->hue[1] = WildHueAllele();
    for (i = 0; i < GENOME_SPEC_PAIRS; i++)   // standing size variation at the A loci; B is never derived in the wild
        g->spec[i] = (OneIn(WILD_SPEC_A_N) ? 1 : 0) | (OneIn(WILD_SPEC_A_N) ? 2 : 0);
}

static u8 PassAllele(u8 alleles, struct BitPool *pool) { return (alleles >> TakeRandomBit(pool)) & 1; }

static u8 PassLethal(const struct Genome *p, struct BitPool *pool)
{
    if (p->lethal[0] == 0)
        return 0;
    if (p->lethal[1] == p->lethal[0] || TakeRandomBit(pool))
        return p->lethal[0];
    return 0;
}

static s8 PassHue(const struct Genome *p, struct BitPool *pool)
{
    s8 a = p->hue[TakeRandomBit(pool)];
    if (OneIn(MUT_HUE_N))
        a += TakeRandomBit(pool) ? 1 : -1;
    if (a > 7) a = 7;
    if (a < -8) a = -8;
    return a;
}

// One gamete's worth of every locus, written into copy `slot` (0 mother, 1 father) of the child.
static void PassGamete(struct Genome *c, const struct Genome *p, u8 slot, struct BitPool *pool)
{
    u32 i, j;
    u8 a;

    for (i = 0; i < NUM_STATS; i++)
        for (j = 0; j < GENOME_IV_LOCI; j++)
            c->iv[i][j] |= PassAllele(p->iv[i][j], pool) << slot;
    for (i = 0; i < NUM_COAT_LOCI; i++)
        c->coat[i] |= PassAllele(p->coat[i], pool) << slot;
    a = PassAllele(p->shiny, pool);
    if (OneIn(MUT_SHINY_N))
        a = 1;                                  // de novo shiny mutation
    c->shiny |= a << slot;
    c->hue[slot] = PassHue(p, pool);
    for (i = 0; i < GENOME_SPEC_LOCI; i++)
    {
        a = PassAllele(p->spec[i], pool);
        if (a == 0 && OneIn(MUT_SPEC_N))        // one-way: ancestral -> derived (sim/speciation_sim.py)
            a = 1;
        c->spec[i] |= a << slot;
    }
}

void InheritGenome(struct Genome *c, const struct Genome *mother, const struct Genome *father)
{
    struct BitPool pool = {0};
    u8 m, f;

    memset(c, 0, sizeof(*c));
    PassGamete(c, mother, 0, &pool);
    PassGamete(c, father, 1, &pool);

    m = PassLethal(mother, &pool);
    f = PassLethal(father, &pool);
    if (m != 0 && m == f)
        c->lethal[0] = c->lethal[1] = m;
    else if (m != 0 && f != 0)
        c->lethal[0] = (Random() & 1) ? f : m;
    else
        c->lethal[0] = m | f;

    // Postzygotic isolation: the egg hatches with probability 2^-n, n = incompatible pairs. Rolled here, once per
    // egg, so the outcome can't be re-rolled at hatch time. Two copies of the lethal also make the egg inviable.
    {
        u32 n = GenomeIncompatiblePairs(c);
        c->dead = (c->lethal[1] != 0) || ((Random() & ((1u << n) - 1)) != 0);
    }
}

void CloneGenome(struct Genome *c, const struct Genome *p) { *c = *p; }

void GenomeFromIVs(struct Genome *g, u8 stat, u8 iv, u32 seed)
{
    u32 target, count, pick, i;

    if (stat >= NUM_STATS)
        return;
    if (iv > MAX_PER_STAT_IVS)
        iv = MAX_PER_STAT_IVS;
    target = (iv * IV_COPIES * 2 + 31) / 62;     // round(iv * 6 / 31)
    count = CountStatCopies(g, stat);
    seed ^= (stat + 1) * 0x9E3779B9;
    while (count != target)
    {
        bool32 adding = (count < target);
        pick = NextSeeded(&seed) % (adding ? (IV_COPIES - count) : count);
        for (i = 0; i < IV_COPIES; i++)
        {
            u8 mask = 1 << (i & 1);
            bool32 isSet = (g->iv[stat][i >> 1] & mask) != 0;
            if (isSet != adding)
            {
                if (pick == 0) { g->iv[stat][i >> 1] ^= mask; break; }
                pick--;
            }
        }
        count += adding ? 1 : -1;
    }
}

void SynthesizeLegacyGenome(struct Genome *g, const u8 *ivs, bool8 shiny, u32 seed)
{
    u32 i;
    memset(g, 0, sizeof(*g));
    for (i = 0; i < NUM_STATS; i++)
        GenomeFromIVs(g, i, ivs[i], seed);
    g->shiny = shiny ? 1 : 0;
}

void MatchGenomeShininess(struct Genome *g, bool8 shiny)
{
    if (shiny && g->shiny == 0)
        g->shiny = 1;
    else if (!shiny)
        g->shiny = 0;
}

bool8 GenomeIsShiny(const struct Genome *g) { return g->shiny != 0; }
bool8 GenomeIsLethal(const struct Genome *g) { return g->dead != 0; }

u8 GenomeCoat(const struct Genome *g)
{
    if (g->coat[COAT_LOCUS_C] == 3) return COAT_ALBINO;
    if (g->coat[COAT_LOCUS_E] == 3) return COAT_RED;
    if (g->coat[COAT_LOCUS_A] == 3) return COAT_BLACK;
    return COAT_NORMAL;
}

u8 GenomeIV(const struct Genome *g, u8 stat)
{
    if (stat >= NUM_STATS)
        return 0;
    return (31 * CountStatCopies(g, stat) + IV_COPIES / 2) / IV_COPIES;
}

s32 GenomeHueShift(const struct Genome *g) { return (g->hue[0] + g->hue[1]) * HUE_STEP; }

static inline bool32 HasAncestral(u8 locus) { return locus != 3; }
static inline bool32 HasDerived(u8 locus) { return locus != 0; }

u32 GenomeSpecDerivedCopies(const struct Genome *g)
{
    u32 i, k = 0;
    for (i = 0; i < GENOME_SPEC_LOCI; i++)
        k += (g->spec[i] & 1) + ((g->spec[i] >> 1) & 1);
    return k;
}

u32 GenomeSize(const struct Genome *g, u32 personality) { return 4 * GenomeSpecDerivedCopies(g) + (personality & 7); }

u32 GenomeSizeScale(const struct Genome *g, u32 personality)
{
    u32 scale = 240 + 4 * GenomeSize(g, personality);   // wild ~1.0x; a finished line (16 copies) 2.0x
    return scale > 512 ? 512 : scale;
}

// The size a sprite is DRAWN at. The GBA scales sprites by repeating pixels, and in-between sizes (1.07x) double a
// stray row/column every few pixels, which looks lumpy. Only sizes whose inverse (256/scale) is a whole number repeat
// pixels in an even pattern: 1x, 1.33x (3 -> 4 px), 1.6x (5 -> 8 px), 2x. The summary HEIGHT stays exact.
u32 GenomeDisplayScale(const struct Genome *g, u32 personality, u32 maxScale)
{
    static const u16 steps[] = {256, 341, 409, 512};   // 65536 / step truncates to 256, 192, 160, 128 exactly
    u32 s = GenomeSizeScale(g, personality), i, best = 0;
    for (i = 1; i < sizeof(steps) / sizeof(steps[0]); i++)
        if (steps[i] <= maxScale && s * s >= (u32)steps[i - 1] * steps[i])   // nearest step on a log scale
            best = i;
    return steps[best];
}

u32 GenomeHeightCm(const struct Genome *g, u32 personality, u32 speciesHeightDm)
{
    return (speciesHeightDm * 10 * GenomeSizeScale(g, personality) + 128) / 256;
}

u32 GenomeIncompatiblePairs(const struct Genome *g)
{
    u32 i, n = 0;
    for (i = 0; i < GENOME_SPEC_PAIRS; i++)
        if (HasAncestral(g->spec[i]) && HasDerived(g->spec[GENOME_SPEC_PAIRS + i]))
            n++;
    return n;
}

u32 GenomeSpeciesDistance(const struct Genome *x, const struct Genome *y)
{
    u32 i, d = 0;
    for (i = 0; i < GENOME_SPEC_PAIRS; i++)
    {
        u8 xa = x->spec[i], ya = y->spec[i], xb = x->spec[GENOME_SPEC_PAIRS + i], yb = y->spec[GENOME_SPEC_PAIRS + i];
        if ((xb == 3 && ya == 0) || (yb == 3 && xa == 0))
            d++;
    }
    return d;
}

u8 GenomeCompatibilityScore(const struct Genome *x, const struct Genome *y)
{
    static const u8 sScoreByD[] = {70, 50, 20, 0};
    u32 d = GenomeSpeciesDistance(x, y);
    return sScoreByD[d > 3 ? 3 : d];
}

static void PutBits(u32 *p, u32 pos, u32 n, u32 v)
{
    u32 i;
    for (i = 0; i < n; i++, pos++)
        if ((v >> i) & 1)
            p[pos / 32] |= 1u << (pos % 32);
}

static u32 GetBits(const u32 *p, u32 pos, u32 n)
{
    u32 i, v = 0;
    for (i = 0; i < n; i++, pos++)
        v |= ((p[pos / 32] >> (pos % 32)) & 1) << i;
    return v;
}

void PackGenome(const struct Genome *g, u32 *p)
{
    u32 i, j;
    for (i = 0; i < GENOME_PACKED_WORDS; i++)
        p[i] = 0;
    for (i = 0; i < NUM_STATS; i++)
        for (j = 0; j < GENOME_IV_LOCI; j++)
            PutBits(p, B_IV + (i * GENOME_IV_LOCI + j) * 2, 2, g->iv[i][j]);
    PutBits(p, B_SHINY, 2, g->shiny);
    for (i = 0; i < NUM_COAT_LOCI; i++)
        PutBits(p, B_COAT + i * 2, 2, g->coat[i]);
    PutBits(p, B_LETHAL, 7, g->lethal[0]);
    PutBits(p, B_DEAD, 1, g->dead != 0);
    PutBits(p, B_HUE, 4, (u8)g->hue[0] & 15);
    PutBits(p, B_HUE + 4, 4, (u8)g->hue[1] & 15);
    for (i = 0; i < GENOME_SPEC_LOCI; i++)
        PutBits(p, B_SPEC + i * 2, 2, g->spec[i]);
    PutBits(p, B_HAS, 1, 1);
}

static s8 SignExtend4(u32 v) { return (v & 8) ? (s8)(v | 0xF0) : (s8)v; }

bool8 UnpackGenome(const u32 *p, struct Genome *g)
{
    u32 i, j;
    memset(g, 0, sizeof(*g));
    for (i = 0; i < NUM_STATS; i++)
        for (j = 0; j < GENOME_IV_LOCI; j++)
            g->iv[i][j] = GetBits(p, B_IV + (i * GENOME_IV_LOCI + j) * 2, 2);
    g->shiny = GetBits(p, B_SHINY, 2);
    for (i = 0; i < NUM_COAT_LOCI; i++)
        g->coat[i] = GetBits(p, B_COAT + i * 2, 2);
    g->lethal[0] = GetBits(p, B_LETHAL, 7);
    g->dead = GetBits(p, B_DEAD, 1);
    g->lethal[1] = 0;   // a carrier; the homozygous case only exists in an egg that is already marked dead
    g->hue[0] = SignExtend4(GetBits(p, B_HUE, 4));
    g->hue[1] = SignExtend4(GetBits(p, B_HUE + 4, 4));
    for (i = 0; i < GENOME_SPEC_LOCI; i++)
        g->spec[i] = GetBits(p, B_SPEC + i * 2, 2);
    return GetBits(p, B_HAS, 1);
}
