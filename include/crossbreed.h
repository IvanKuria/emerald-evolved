#ifndef GUARD_CROSSBREED_H
#define GUARD_CROSSBREED_H

// Cross-breeding (research/pokemon-crossbreed/GAME_SPEC.md, GAME_REPORT.md).
// A hybrid's body is its own species (the mother's line); its LINEAGE names the donor species that gave it colours and
// body parts. donor[0] = the newest father (colours, one part, a type, its moves); donor[1] = an older donor carried by
// the mother's line (its part, its type for display, its moves). Storage: GetBoxMonLineage in pokemon.c (21 bits).

#include "constants/pokemon.h"

#define LINEAGE_DONORS 2

struct Lineage
{
    u16 donor[LINEAGE_DONORS];   // the donor's own species (forms folded: a Pikachu father is Pikachu, not Pichu);
                                 // SPECIES_NONE = none; donor[0] is always filled first
    bool8 fertile;               // females only: can this hybrid make eggs (Haldane: males never can)
};

struct BoxPokemon;
struct Pokemon;
struct LevelUpMove;

void GetBoxMonLineage(const struct BoxPokemon *boxMon, struct Lineage *lineage);   // pokemon.c
void SetBoxMonLineage(struct BoxPokemon *boxMon, const struct Lineage *lineage);   // pokemon.c

u16 CrossbreedFamily(u16 species);                       // egg species of the family; Nidoran merged, Volbeat/Illumise not
bool32 LineageIsHybrid(const struct Lineage *lineage);
bool32 IsBoxMonHybrid(const struct BoxPokemon *boxMon);
bool32 IsBoxMonMaleHybrid(const struct BoxPokemon *boxMon);
bool32 IsBoxMonFertile(const struct BoxPokemon *boxMon);  // TRUE for every non-hybrid

// Feature 1: chromosome counts
u32 GetSpeciesChromosomes(u16 species);
u32 GetLineageChromosomes(u16 body, const struct Lineage *lineage);
u32 GetBoxMonChromosomes(const struct BoxPokemon *boxMon);
u32 HybridFertilityChance256(u32 motherChromosomes, u32 fatherChromosomes);   // /256

// Feature 6: parent-of-origin growth (x256, 256 = no effect), applied on top of the genetic size
u32 GetLineageGrowth256(u16 body, const struct Lineage *lineage);
u32 BoxMonDisplayScale(const struct BoxPokemon *boxMon, u32 maxScale);   // drawn size, clean pixel steps
u32 BoxMonHeightCm(const struct BoxPokemon *boxMon);

// Feature 4: types. Battle typing = the first two; a third (an older donor's) is display only.
u32 GetBoxMonHybridTypes(const struct BoxPokemon *boxMon, u16 species, u8 *types);   // returns 1..3
void GetBoxMonBattleTypes(const struct BoxPokemon *boxMon, u16 species, u8 *type1, u8 *type2);
void CrossbreedSetBattlerTypes(u32 battler);   // after a battler's species types are set

// Feature 5: the level-up learnset of a mon: its species' plus every donor's, by level. Non-hybrids: the species'.
const struct LevelUpMove *CrossbreedLearnset(const struct BoxPokemon *boxMon, u16 species);

// Features 2, 7, 9: the child's lineage. hatchChance256 = 256 normally, lower for a three-species attempt.
void MakeChildLineage(const struct BoxPokemon *mother, const struct BoxPokemon *father, u16 body, bool32 childIsFemale,
                      struct Lineage *child, u32 *hatchChance256);
bool32 IsThirdSpeciesCross(const struct BoxPokemon *mother, const struct BoxPokemon *father);

// Feature 8: wild hybrids
#define WILD_HYBRID_CHANCE 16   // 1 in N encounters on a route with a related pair
#define WILD_HYBRID_MAX_CHROMOSOME_GAP 6
bool32 WildHybridPartnerOk(u16 mother, u16 father);
bool32 TryMakeWildHybridFromTable(struct Pokemon *mon, const u16 *speciesTable, u32 count);
#if EVO_TEST
extern bool8 gCrossbreedForceWildHybrid;
#endif
bool32 IsWildHybridName(const u8 *name);
void IdentifyHybrid(struct Pokemon *mon);   // a caught "???" gets its species name

// Specials
void CrossbreedIsSelectedMonMaleHybrid(void);   // VAR_RESULT for the Day Care deposit script

#endif // GUARD_CROSSBREED_H
