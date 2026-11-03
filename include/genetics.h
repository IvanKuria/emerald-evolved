#ifndef GUARD_GENETICS_H
#define GUARD_GENETICS_H

// Real genetics, evolution edition (port of pokeemerald-real-genetics onto pokeemerald-expansion 1.17.2).
// Diploid: each locus holds two allele bits, bit 0 = copy from the mother, bit 1 = copy from the father.
// Genome v3 (77 bits, layout in research/pokemon-evolution/BUILD_STATUS.md): 3 IV loci per stat, coat C/E/A, dominant
// shiny MUTATION, one carried lethal + an "inviable egg" bit, a codominant hue gene (two 4-bit signed alleles), and
// 8 speciation loci (4 Dobzhansky-Muller pairs A1-4 / B1-4) that also set body size (sim/SPECIATION.md).

#define GENOME_IV_LOCI      3
#define GENOME_SPEC_PAIRS   4
#define GENOME_SPEC_LOCI    (GENOME_SPEC_PAIRS * 2)   // loci 0-3 = A1-A4, 4-7 = B1-B4
#define GENOME_LETHAL_SLOTS 2
#define GENOME_LETHAL_LOCI  127
#define GENOME_PACKED_BITS  77
#define GENOME_PACKED_WORDS 3

enum { COAT_LOCUS_C, COAT_LOCUS_E, COAT_LOCUS_A, NUM_COAT_LOCI };
enum { COAT_NORMAL, COAT_ALBINO, COAT_RED, COAT_BLACK };

struct Genome
{
    u8 iv[NUM_STATS][GENOME_IV_LOCI];
    u8 coat[NUM_COAT_LOCI];
    u8 shiny;                       // two allele bits, 1 = the shiny mutation; DOMINANT (one copy = shiny)
    u8 lethal[GENOME_LETHAL_SLOTS];
    u8 dead;                        // inviable egg (two copies of the lethal, or a failed hybrid): never hatches
    s8 hue[2];                      // hue alleles -8..7 (0 = wild type), [0] mother copy, [1] father copy
    u8 spec[GENOME_SPEC_LOCI];      // speciation loci, two allele bits, 1 = derived; every derived copy = bigger
};

struct BoxPokemon;
void GetBoxMonGenome(const struct BoxPokemon *boxMon, struct Genome *genome);
void SetBoxMonGenome(struct BoxPokemon *boxMon, const struct Genome *genome);

void GenerateWildGenome(struct Genome *genome);
void InheritGenome(struct Genome *child, const struct Genome *mother, const struct Genome *father);
void CloneGenome(struct Genome *child, const struct Genome *parent);
void GenomeFromIVs(struct Genome *genome, u8 stat, u8 iv, u32 seed);
void SynthesizeLegacyGenome(struct Genome *genome, const u8 *ivs, bool8 shiny, u32 seed);
void MatchGenomeShininess(struct Genome *genome, bool8 shiny);

bool8 GenomeIsShiny(const struct Genome *genome);
bool8 GenomeIsLethal(const struct Genome *genome);
u8 GenomeCoat(const struct Genome *genome);
u8 GenomeIV(const struct Genome *genome, u8 stat);
s32 GenomeHueShift(const struct Genome *genome);   // hue units, 1536 = 360 degrees
u32 GenomeSpecDerivedCopies(const struct Genome *genome);         // 0..16
u32 GenomeSize(const struct Genome *genome, u32 personality);     // 4 * derived copies + (personality & 7), 0..71
u32 GenomeSizeScale(const struct Genome *genome, u32 personality); // x256: 240 + 4 * size, capped at 512 (2x)
// individual height in cm = species Pokédex height (decimeters) x 10 x GenomeSizeScale / 256, rounded
u32 GenomeHeightCm(const struct Genome *genome, u32 personality, u32 speciesHeightDm);
u32 GenomeIncompatiblePairs(const struct Genome *genome);         // n: pairs with a derived B next to an ancestral A
u32 GenomeSpeciesDistance(const struct Genome *x, const struct Genome *y); // d: completed opposite pairs
u8 GenomeCompatibilityScore(const struct Genome *x, const struct Genome *y); // 70 / 50 / 20 / 0 for d = 0 / 1 / 2 / >=3

void PackGenome(const struct Genome *genome, u32 *packed);
bool8 UnpackGenome(const u32 *packed, struct Genome *genome);

#endif // GUARD_GENETICS_H
