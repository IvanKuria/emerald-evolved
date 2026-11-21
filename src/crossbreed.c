// Cross-breeding (research/pokemon-crossbreed/GAME_SPEC.md; every rule below is listed in GAME_REPORT.md).
// Pure rules + the Day Care / wild / summary glue. Storage of the lineage is GetBoxMonLineage in pokemon.c.
#include "global.h"
#include "crossbreed.h"
#include "battle.h"
#include "daycare.h"
#include "event_data.h"
#include "genetics.h"
#include "pokemon.h"
#include "random.h"
#include "string_util.h"
#include "constants/pokemon.h"
#include "constants/species.h"

// ---------------------------------------------------------------- families
// The family a species breeds as: the egg species of its line, forms folded. Nidoran F/M are one species split by sex.
// Volbeat and Illumise are kept apart: they are the spec's example of two related species sharing a route.
u16 CrossbreedFamily(u16 species)
{
    if (species == SPECIES_NONE)
        return SPECIES_NONE;
    species = GET_BASE_SPECIES_ID(GetEggSpecies(GET_BASE_SPECIES_ID(species)));
    if (species == SPECIES_NIDORAN_M)
        species = SPECIES_NIDORAN_F;
    return species;
}

bool32 LineageIsHybrid(const struct Lineage *l) { return l->donor[0] != SPECIES_NONE; }

bool32 IsBoxMonHybrid(const struct BoxPokemon *boxMon)
{
    struct Lineage l;
    GetBoxMonLineage(boxMon, &l);
    return LineageIsHybrid(&l);
}

// Haldane's rule applies to first crosses (F1): an F1 son is always sterile and the Day Care refuses him. Once a fertile
// hybrid female founds a LINE (backcrosses, the three-species line), the line is fertile: its sons and daughters carry
// the fertile bit and breed. So the fertile bit means "this hybrid can breed", for either sex; only F1 sons never get it.
bool32 IsBoxMonMaleHybrid(const struct BoxPokemon *boxMon)   // a sterile (F1) male hybrid
{
    struct Lineage l;
    GetBoxMonLineage(boxMon, &l);
    return LineageIsHybrid(&l) && !l.fertile && GetBoxMonGender((struct BoxPokemon *)boxMon) == MON_MALE;
}

bool32 IsBoxMonFertile(const struct BoxPokemon *boxMon)
{
    struct Lineage l;
    GetBoxMonLineage(boxMon, &l);
    if (!LineageIsHybrid(&l))
        return TRUE;
    return l.fertile;
}

// Two hybrids of one line count as the same kind: same body family and the same donor families.
bool32 AreBoxMonsSameHybridLine(const struct BoxPokemon *a, const struct BoxPokemon *b)
{
    struct Lineage la, lb;
    u32 i, j, na = 0, nb = 0;

    GetBoxMonLineage(a, &la);
    GetBoxMonLineage(b, &lb);
    if (!LineageIsHybrid(&la) || !LineageIsHybrid(&lb))
        return FALSE;
    if (CrossbreedFamily(GetBoxMonData((struct BoxPokemon *)a, MON_DATA_SPECIES)) != CrossbreedFamily(GetBoxMonData((struct BoxPokemon *)b, MON_DATA_SPECIES)))
        return FALSE;
    for (i = 0; i < LINEAGE_DONORS; i++)
    {
        bool32 found = FALSE;
        if (la.donor[i] == SPECIES_NONE)
            continue;
        na++;
        for (j = 0; j < LINEAGE_DONORS; j++)
            if (lb.donor[j] != SPECIES_NONE && CrossbreedFamily(lb.donor[j]) == CrossbreedFamily(la.donor[i]))
                found = TRUE;
        if (!found)
            return FALSE;
    }
    for (j = 0; j < LINEAGE_DONORS; j++)
        nb += lb.donor[j] != SPECIES_NONE;
    return na == nb;
}

// ---------------------------------------------------------------- feature 1: chromosomes
// Rule (documented in GAME_REPORT.md): a family's number = base of its first egg group + 2 * (dex no. of the family
// % 5) - 4, so families in one egg group sit within 8 of each other and different groups sit further apart. Every
// member of a family shares it. Fixed numbers from the video override the rule (Raichu is the one species override).
static const u8 sEggGroupChromosomeBase[] =
{
    [EGG_GROUP_NONE]                = 56,
    [EGG_GROUP_MONSTER]             = 50,
    [EGG_GROUP_WATER_1]             = 58,
    [EGG_GROUP_BUG]                 = 66,
    [EGG_GROUP_FLYING]              = 78,
    [EGG_GROUP_FIELD]               = 40,
    [EGG_GROUP_FAIRY]               = 32,
    [EGG_GROUP_GRASS]               = 24,
    [EGG_GROUP_HUMAN_LIKE]          = 46,
    [EGG_GROUP_WATER_3]             = 88,
    [EGG_GROUP_MINERAL]             = 16,
    [EGG_GROUP_AMORPHOUS]           = 30,
    [EGG_GROUP_WATER_2]             = 100,
    [EGG_GROUP_DITTO]               = 48,
    [EGG_GROUP_DRAGON]              = 62,
    [EGG_GROUP_NO_EGGS_DISCOVERED]  = 56,
};

static const struct { u16 family; u8 n; } sFamilyChromosomes[] =
{
    {SPECIES_PICHU, 40}, {SPECIES_CHARMANDER, 52}, {SPECIES_BULBASAUR, 48}, {SPECIES_EEVEE, 38},
    {SPECIES_MAGIKARP, 104}, {SPECIES_WAILMER, 44},
};

static u32 FamilyEggGroup(u16 family)
{
    u32 group = gSpeciesInfo[family].eggGroups[0], i;
    const struct Evolution *evos;

    // a baby (Pichu, Azurill...) is in no egg group; its line is in its evolution's
    for (i = 0; i < 3 && group == EGG_GROUP_NO_EGGS_DISCOVERED; i++)
    {
        evos = GetSpeciesEvolutions(family);
        if (evos == NULL || evos[0].method == EVOLUTIONS_END)
            break;
        family = evos[0].targetSpecies;
        group = gSpeciesInfo[family].eggGroups[0];
    }
    return group < ARRAY_COUNT(sEggGroupChromosomeBase) ? group : EGG_GROUP_NONE;
}

u32 GetSpeciesChromosomes(u16 species)
{
    u32 i, family;

    if (species == SPECIES_NONE)
        return 0;
    if (GET_BASE_SPECIES_ID(species) == SPECIES_RAICHU)
        return 42;
    family = CrossbreedFamily(species);
    for (i = 0; i < ARRAY_COUNT(sFamilyChromosomes); i++)
        if (sFamilyChromosomes[i].family == family)
            return sFamilyChromosomes[i].n;
    return sEggGroupChromosomeBase[FamilyEggGroup(family)] + 2 * (SpeciesToNationalPokedexNum(family) % 5) - 4;
}

// A hybrid has half its chromosomes from each parent. The mother's side of a two-donor line is itself a mix.
static u32 MotherSideChromosomes(u16 body, const struct Lineage *l)
{
    if (l->donor[1] != SPECIES_NONE)
        return (GetSpeciesChromosomes(body) + GetSpeciesChromosomes(l->donor[1])) / 2;
    return GetSpeciesChromosomes(body);
}

u32 GetLineageChromosomes(u16 body, const struct Lineage *l)
{
    if (!LineageIsHybrid(l))
        return GetSpeciesChromosomes(body);
    return MotherSideChromosomes(body, l) / 2 + GetSpeciesChromosomes(l->donor[0]) / 2;
}

u32 GetBoxMonChromosomes(const struct BoxPokemon *boxMon)
{
    struct Lineage l;
    GetBoxMonLineage(boxMon, &l);
    return GetLineageChromosomes(GetBoxMonData((struct BoxPokemon *)boxMon, MON_DATA_SPECIES), &l);
}

// Feature 7: a female hybrid's chance of being fertile falls with the parents' chromosome gap d: 256 * (12 - d) / 12,
// so d = 2 -> 83%, d = 4 (Charmander x Bulbasaur) -> 67%, and d >= 12 (Pikachu x Charmander) -> 1 in 256 ("the mule").
u32 HybridFertilityChance256(u32 cm, u32 cf)
{
    u32 d = cm > cf ? cm - cf : cf - cm;
    if (d >= 12)
        return 1;
    return 256 - d * 256 / 12;
}

// ---------------------------------------------------------------- feature 6: parent-of-origin growth
// Every family passes down an imprinted growth gene. The copy from the father is the accelerator, the copy from the
// mother the brake, and within a species they cancel. Its strength is the family's chromosome count (the growth gene is
// a dosage effect). In a hybrid the net push is d = father's strength - mother's, and the body grows 2^(d / 12):
// Charmander (52) father x Pikachu (40) mother -> 2x (bigger than both parents); the reverse -> 0.5x (smaller than both).
static const u16 sGrowthPow[13] = {256, 271, 287, 304, 323, 342, 362, 384, 406, 431, 456, 483, 512};   // 256*2^(i/12)

u32 GetLineageGrowth256(u16 body, const struct Lineage *l)
{
    s32 d;
    if (!LineageIsHybrid(l))
        return 256;
    d = (s32)GetSpeciesChromosomes(l->donor[0]) - (s32)MotherSideChromosomes(body, l);
    if (d > 12) d = 12;
    if (d < -12) d = -12;
    return d >= 0 ? sGrowthPow[d] : (256 * 256) / sGrowthPow[-d];
}

static u32 BoxMonGrowth256(const struct BoxPokemon *boxMon)
{
    struct Lineage l;
    GetBoxMonLineage(boxMon, &l);
    return GetLineageGrowth256(GetBoxMonData((struct BoxPokemon *)boxMon, MON_DATA_SPECIES), &l);
}

u32 BoxMonDisplayScale(const struct BoxPokemon *boxMon, u32 maxScale)
{
    struct Genome g;
    GetBoxMonGenome(boxMon, &g);
    return GenomeDisplayScaleGrowth(&g, boxMon->personality, BoxMonGrowth256(boxMon), maxScale);
}

u32 BoxMonHeightCm(const struct BoxPokemon *boxMon)
{
    struct Genome g;
    u16 species = GetBoxMonData((struct BoxPokemon *)boxMon, MON_DATA_SPECIES);
    GetBoxMonGenome(boxMon, &g);
    return (GenomeHeightCm(&g, boxMon->personality, gSpeciesInfo[species].height) * BoxMonGrowth256(boxMon) + 128) / 256;
}

// ---------------------------------------------------------------- feature 4: types
// One type from each parent: the body's primary + the newest father's primary (if those match, the body's secondary).
// An older donor's primary is a third type, shown on the summary and inherited, but battles use the first two.
u32 GetBoxMonHybridTypes(const struct BoxPokemon *boxMon, u16 species, u8 *types)
{
    struct Lineage l;
    u32 n;
    u8 t;

    types[0] = GetSpeciesType(species, 0);
    types[1] = GetSpeciesType(species, 1);
    n = (types[1] != types[0]) ? 2 : 1;
    if (boxMon == NULL)
        return n;
    GetBoxMonLineage(boxMon, &l);
    if (!LineageIsHybrid(&l))
        return n;
    t = GetSpeciesType(l.donor[0], 0);
    if (t != types[0])
        types[1] = t;
    n = (types[1] != types[0]) ? 2 : 1;
    if (l.donor[1] != SPECIES_NONE)
    {
        t = GetSpeciesType(l.donor[1], 0);
        if (t != types[0] && t != types[1])
        {
            // display order: body, the older donor, the newest donor (FIRE / GRASS / ELECTR for the video's line)
            if (n == 2)
            {
                types[2] = types[1];
                types[1] = t;
                n = 3;
            }
            else
            {
                types[n++] = t;
            }
        }
    }
    return n;
}

void GetBoxMonBattleTypes(const struct BoxPokemon *boxMon, u16 species, u8 *type1, u8 *type2)
{
    struct Lineage l;
    u8 t;

    *type1 = GetSpeciesType(species, 0);
    *type2 = GetSpeciesType(species, 1);
    if (boxMon == NULL)
        return;
    GetBoxMonLineage(boxMon, &l);
    if (!LineageIsHybrid(&l))
        return;
    t = GetSpeciesType(l.donor[0], 0);
    if (t != *type1)
        *type2 = t;
}

void CrossbreedSetBattlerTypes(u32 battler)
{
    u8 t1, t2;
    struct Pokemon *mon = GetBattlerMon(battler);
    if (mon == NULL || gBattleMons[battler].species == SPECIES_NONE)
        return;
    if (GetMonData(mon, MON_DATA_SPECIES) != gBattleMons[battler].species)   // transformed / illusion: not its own body
        return;
    GetBoxMonBattleTypes(&mon->box, gBattleMons[battler].species, &t1, &t2);
    gBattleMons[battler].types[0] = t1;
    gBattleMons[battler].types[1] = t2;
}

// ---------------------------------------------------------------- feature 5: moves from both sides
#define MERGED_LEARNSET_MAX 160
static EWRAM_DATA struct LevelUpMove sMergedLearnset[MERGED_LEARNSET_MAX + 1] = {0};

const struct LevelUpMove *CrossbreedLearnset(const struct BoxPokemon *boxMon, u16 species)
{
    const struct LevelUpMove *sets[1 + LINEAGE_DONORS];
    u32 pos[1 + LINEAGE_DONORS] = {0};
    u32 nSets = 1, n = 0, i, j;
    struct Lineage l;

    sets[0] = GetSpeciesLevelUpLearnset(species);
    if (boxMon == NULL)
        return sets[0];
    GetBoxMonLineage(boxMon, &l);
    if (!LineageIsHybrid(&l))
        return sets[0];
    for (i = 0; i < LINEAGE_DONORS; i++)
        if (l.donor[i] != SPECIES_NONE)
            sets[nSets++] = GetSpeciesLevelUpLearnset(l.donor[i]);
    for (i = 1; i < nSets; i++)   // a donor's evolution moves (level 0) belong to its own evolution, not the hybrid's
        while (sets[i][pos[i]].move != LEVEL_UP_MOVE_END && sets[i][pos[i]].level == 0)
            pos[i]++;
    // merge by level; at one level the body's moves come first; a move already listed is skipped
    while (n < MERGED_LEARNSET_MAX)
    {
        u32 best = nSets, bestLevel = 0xFFFF;
        for (i = 0; i < nSets; i++)
            if (sets[i][pos[i]].move != LEVEL_UP_MOVE_END && sets[i][pos[i]].level < bestLevel)
                best = i, bestLevel = sets[i][pos[i]].level;
        if (best == nSets)
            break;
        for (j = 0; j < n; j++)
            if (sMergedLearnset[j].move == sets[best][pos[best]].move)
                break;
        if (j == n)
            sMergedLearnset[n++] = sets[best][pos[best]];
        pos[best]++;
    }
    sMergedLearnset[n].move = LEVEL_UP_MOVE_END;
    sMergedLearnset[n].level = 0;
    return sMergedLearnset;
}

// ---------------------------------------------------------------- features 2, 7, 9: the child's lineage
static bool32 FamilyInLineage(u16 family, u16 body, const struct Lineage *l)
{
    u32 i;
    if (family == CrossbreedFamily(body))
        return TRUE;
    for (i = 0; i < LINEAGE_DONORS; i++)
        if (l->donor[i] != SPECIES_NONE && CrossbreedFamily(l->donor[i]) == family)
            return TRUE;
    return FALSE;
}

// A fertile hybrid female crossed with a species from outside her line: her chromosomes are a mix, so the cross can
// work, but rarely (the egg is laid, and hatches 1 in THIRD_SPECIES_HATCH_N).
#define THIRD_SPECIES_HATCH_N 8
bool32 IsThirdSpeciesCross(const struct BoxPokemon *mother, const struct BoxPokemon *father)
{
    struct Lineage lm;
    u16 body = GetBoxMonData((struct BoxPokemon *)mother, MON_DATA_SPECIES);
    u16 dad = GetBoxMonData((struct BoxPokemon *)father, MON_DATA_SPECIES);
    GetBoxMonLineage(mother, &lm);
    return LineageIsHybrid(&lm) && !FamilyInLineage(CrossbreedFamily(dad), body, &lm);
}

static void AddDonor(struct Lineage *c, u16 body, u16 donor)
{
    u32 i;
    if (donor == SPECIES_NONE || CrossbreedFamily(donor) == CrossbreedFamily(body))
        return;
    for (i = 0; i < LINEAGE_DONORS; i++)
    {
        if (c->donor[i] == SPECIES_NONE)
        {
            c->donor[i] = donor;
            return;
        }
        if (CrossbreedFamily(c->donor[i]) == CrossbreedFamily(donor))
            return;
    }
}

// body = the egg's species (the mother's line). The newest father becomes donor[0]; the mother's own donors follow
// (only LINEAGE_DONORS are kept: the oldest part drops off a fourth-species cross).
void MakeChildLineage(const struct BoxPokemon *mother, const struct BoxPokemon *father, u16 body, bool32 childIsFemale,
                      struct Lineage *c, u32 *hatchChance256)
{
    struct Lineage lm, lf;
    u16 dad = GET_BASE_SPECIES_ID(GetBoxMonData((struct BoxPokemon *)father, MON_DATA_SPECIES));
    u32 i;

    memset(c, 0, sizeof(*c));
    *hatchChance256 = 256;
    GetBoxMonLineage(mother, &lm);
    GetBoxMonLineage(father, &lf);
    AddDonor(c, body, dad);
    for (i = 0; i < LINEAGE_DONORS; i++)
        AddDonor(c, body, lm.donor[i]);
    for (i = 0; i < LINEAGE_DONORS; i++)
        AddDonor(c, body, lf.donor[i]);
    if (IsThirdSpeciesCross(mother, father))   // sons and daughters alike: most of these eggs never hatch
        *hatchChance256 = 256 / THIRD_SPECIES_HATCH_N;
    if (!LineageIsHybrid(c))
        return;
    if (!LineageIsHybrid(&lm) && CrossbreedFamily(dad) != CrossbreedFamily(GetBoxMonData((struct BoxPokemon *)mother, MON_DATA_SPECIES)))
    {
        // a first cross (F1): a pure mother and a father of another species. Haldane's rule: sons are sterile;
        // daughters are fertile by the parents' chromosome gap.
        if (childIsFemale)
            c->fertile = (Random() & 0xFF) < HybridFertilityChance256(GetBoxMonChromosomes(mother), GetBoxMonChromosomes(father));
    }
    else
    {
        // inside a fertile line (a hybrid mother, or a pure mother of the line's own body with a line father; the
        // three-species line too): the line is a working mix, so sons and daughters both breed
        c->fertile = TRUE;
    }
}

// ---------------------------------------------------------------- feature 8: wild hybrids
// padded to a full name: MON_DATA_NICKNAME copies POKEMON_NAME_LENGTH bytes whatever the terminator
static const u8 sText_WildHybridName[POKEMON_NAME_LENGTH + 1] = _("???$$$$$$$$$");

bool32 IsWildHybridName(const u8 *name) { return StringCompare(name, sText_WildHybridName) == 0; }

static bool32 CanBeMother(u16 species) { u8 r = gSpeciesInfo[species].genderRatio; return r != MON_MALE && r != MON_GENDERLESS; }
static bool32 CanBeFather(u16 species) { u8 r = gSpeciesInfo[species].genderRatio; return r != MON_FEMALE && r != MON_GENDERLESS; }

static bool32 Breedable(u16 species)
{
    u32 g0 = gSpeciesInfo[species].eggGroups[0];
    return g0 != EGG_GROUP_NO_EGGS_DISCOVERED && g0 != EGG_GROUP_DITTO;
}

static bool32 ShareEggGroup(u16 a, u16 b)
{
    u32 i, j;
    for (i = 0; i < EGG_GROUPS_PER_MON; i++)
        for (j = 0; j < EGG_GROUPS_PER_MON; j++)
            if (gSpeciesInfo[a].eggGroups[i] == gSpeciesInfo[b].eggGroups[j])
                return TRUE;
    return FALSE;
}

// Two species are "closely related" for a wild hybrid when they share an egg group and their chromosome numbers are
// within WILD_HYBRID_MAX_CHROMOSOME_GAP. Route 117: Illumise (70) x Volbeat (68), Oddish (26) x Seedot (26).
bool32 WildHybridPartnerOk(u16 mother, u16 father)
{
    u32 cm, cf;
    if (CrossbreedFamily(mother) == CrossbreedFamily(father) || !Breedable(mother) || !Breedable(father))
        return FALSE;
    if (!CanBeMother(mother) || !CanBeFather(father) || !ShareEggGroup(mother, father))
        return FALSE;
    cm = GetSpeciesChromosomes(mother);
    cf = GetSpeciesChromosomes(father);
    return (cm > cf ? cm - cf : cf - cm) <= WILD_HYBRID_MAX_CHROMOSOME_GAP;
}

#if EVO_TEST
EWRAM_DATA bool8 gCrossbreedForceWildHybrid = FALSE;
#endif

// mon = the wild Pokémon just made; table/count = the encounter table it came from. 1 in WILD_HYBRID_CHANCE, if the
// table holds a related species that could be its father, it becomes their hybrid: same body, the father's lineage.
bool32 TryMakeWildHybridFromTable(struct Pokemon *mon, const u16 *speciesTable, u32 count)
{
    u16 body = GetMonData(mon, MON_DATA_SPECIES), partners[16];
    u32 n = 0, i, j;
    struct Lineage l;

#if EVO_TEST
    if (!gCrossbreedForceWildHybrid)
#endif
    if (Random() % WILD_HYBRID_CHANCE != 0)
        return FALSE;
    for (i = 0; i < count && n < ARRAY_COUNT(partners); i++)
    {
        u16 s = speciesTable[i];
        if (s == SPECIES_NONE || !WildHybridPartnerOk(body, s))
            continue;
        for (j = 0; j < n; j++)
            if (partners[j] == s)
                break;
        if (j == n)
            partners[n++] = s;
    }
    if (n == 0)
        return FALSE;
    memset(&l, 0, sizeof(l));
    l.donor[0] = GET_BASE_SPECIES_ID(partners[Random() % n]);
    if (GetMonGender(mon) == MON_FEMALE)
        l.fertile = (Random() & 0xFF) < HybridFertilityChance256(GetSpeciesChromosomes(body), GetSpeciesChromosomes(l.donor[0]));
    SetBoxMonLineage(&mon->box, &l);
    GiveMonInitialMoveset(mon);                              // both parents' moves at its level
    SetMonData(mon, MON_DATA_NICKNAME, sText_WildHybridName);   // "Wild ??? appeared!" until it is caught
    return TRUE;
}

void IdentifyHybrid(struct Pokemon *mon)
{
    u8 name[POKEMON_NAME_LENGTH + 1];
    GetMonData(mon, MON_DATA_NICKNAME, name);
    if (IsWildHybridName(name) && IsBoxMonHybrid(&mon->box))
    {
        StringCopy_Nickname(name, GetSpeciesName(GetMonData(mon, MON_DATA_SPECIES)));
        SetMonData(mon, MON_DATA_NICKNAME, name);
    }
}

// ---------------------------------------------------------------- Day Care specials
void CrossbreedIsSelectedMonMaleHybrid(void)
{
    struct BoxPokemon *boxMon = GetSelectedBoxMonFromPcOrParty();
    gSpecialVar_Result = boxMon != NULL && IsBoxMonMaleHybrid(boxMon);
}
