// In-ROM test driver for Emerald Evolved (EVO_TEST=1 builds only; writes pokeemerald_evotest.gba). Talks to the
// headless mGBA runner through gEvoTest (log, screenshot and save-dump requests), like realgen_test.c.
#include "global.h"
#if EVO_TEST
#include "main.h"
#include "pokemon.h"
#include "genetics.h"
#include "daycare.h"
#include "overworld.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_setup.h"
#include "battle_gimmick.h"
#include "battle_dynamax.h"
#include "battle_terastal.h"
#include "palette.h"
#include "event_data.h"
#include "random.h"
#include "sprite.h"
#include "script.h"
#include "save.h"
#include "item.h"
#include "pokedex.h"
#include "field_player_avatar.h"
#include "evo_test.h"
#include "sl_test.h"
#include "event_object_movement.h"
#include "genetics_palette.h"
#include "constants/species.h"
#include "constants/maps.h"
#include "constants/flags.h"
#include "constants/vars.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/characters.h"
#include "string_util.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "field_screen_effect.h"
#include "egg_hatch.h"
#include "evolution_scene.h"
#include "pokemon_storage_system.h"
#include "hybrid_sprite.h"
#include "decompress.h"
#include "crossbreed.h"
#include "wild_encounter.h"
#include "strings.h"
#include "constants/abilities.h"

extern const u8 Route117_EventScript_CheckOnTwoMons[];
extern const u8 Route117_PokemonDayCare_EventScript_RefuseMaleHybrid[];

#define LOG_SIZE 2048
struct EvoTestMailbox
{
    u32 magic; u16 shotSeq; u8 finished; u8 pad; u32 logLen; u32 fails; u32 checks; u8 shotName[32]; u8 log[LOG_SIZE];
    u16 saveSeq; u16 pad2; u8 saveName[32];   // offsets 2100 / 2104 (evo_test.lua)
};
EWRAM_DATA struct EvoTestMailbox gEvoTest = {0};

static EWRAM_DATA u16 sStage = 0, sSub = 0, sHoldKeys = 0, sHoldFrames = 0, sShotIndex = 0, sStateTicks = 0;
static EWRAM_DATA u32 sTimer = 0, sCount = 0;
static EWRAM_DATA bool8 sHoldFirst = FALSE;
static EWRAM_DATA u8 sLastHatchState = 0, sWalkDir = 0;
static EWRAM_DATA struct Genome sEggGenome[PARTY_SIZE] = {0};
static EWRAM_DATA struct Pokemon sDeadEgg = {0};

// the real Day Care line (Eevee), kept across frames
#define LINE_EGGS     10
#define LINE_MAX_GENS 60
static EWRAM_DATA struct Pokemon sMum = {0}, sDad = {0}, sGen10[2] = {0}, sFinal[2] = {0}, sWildEevee = {0}, sWildF = {0};
static EWRAM_DATA u16 sLineGen = 0, sLineDoneGen = 0, sLineEggs = 0, sLineFailed = 0, sLineAttempt = 0;
static EWRAM_DATA u16 sLineStalls = 0;   // generations where all 10 eggs failed: the parents (and their counter) carry over
#define LINE_WANT_GENS 24   // the capture saves want a line that finished around generation 20
// pure-logic replicate lines
#define REP_LINES 200
static EWRAM_DATA u8 sRepGens[REP_LINES] = {0};

static void LogStr(const char *s) { while (*s) gEvoTest.log[gEvoTest.logLen++ % LOG_SIZE] = *s++; }
static void LogNum(s32 n)
{
    char b[12]; int i = 11; u32 u = n < 0 ? -n : n;
    b[i] = 0;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (n < 0) b[--i] = '-';
    LogStr(&b[i]);
}
static void Check(bool32 ok, const char *what) { gEvoTest.checks++; if (!ok) gEvoTest.fails++; LogStr(ok ? "  ok    " : "  FAIL  "); LogStr(what); LogStr("\n"); }
static void Shot(const char *name)
{
    int i = 0; u16 n = ++sShotIndex;
    gEvoTest.shotName[i++] = '0' + (n / 10) % 10; gEvoTest.shotName[i++] = '0' + n % 10; gEvoTest.shotName[i++] = '_';
    while (*name && i < 31) gEvoTest.shotName[i++] = *name++;
    gEvoTest.shotName[i] = 0; gEvoTest.shotSeq++;
    LogStr("  shot  "); LogStr((const char *)gEvoTest.shotName); LogStr("\n");
}
static void DumpSave(const char *name)
{
    int i = 0;
    while (*name && i < 31) gEvoTest.saveName[i++] = *name++;
    gEvoTest.saveName[i] = 0; gEvoTest.saveSeq++;
    LogStr("  save  "); LogStr((const char *)gEvoTest.saveName); LogStr("\n");
}
static void Press(u16 k) { gMain.heldKeysRaw |= k; gMain.newKeysRaw |= k; gMain.heldKeys |= k; gMain.newKeys |= k; gMain.newAndRepeatedKeys |= k; }
static void Hold(u16 k, u16 f) { sHoldKeys = k; sHoldFrames = f; sHoldFirst = TRUE; }
static void ApplyHold(void)
{
    if (sHoldFrames == 0) return;
    if (sHoldFirst) Press(sHoldKeys);
    sHoldFirst = FALSE; gMain.heldKeysRaw |= sHoldKeys; gMain.heldKeys |= sHoldKeys; sHoldFrames--;
}
static bool8 FieldIdle(void)
{
    return gMain.callback2 == CB2_Overworld && gMain.callback1 == CB1_Overworld && !ArePlayerFieldControlsLocked() && !gPaletteFade.active && sHoldFrames == 0;
}
static bool8 MashBUntilIdle(void)
{
    sTimer++;
    if (sTimer > 30 && FieldIdle()) return TRUE;
    if ((sTimer % 60) == 0) Press(B_BUTTON);
    if (sTimer > 60 * 120) { Check(FALSE, "timed out waiting for the field"); return TRUE; }
    return FALSE;
}
static void NextStage(void) { sStage++; sSub = 0; sTimer = 0; sCount = 0; }

// female: personality low byte 0; male: 0xFF. genes: NULL = wild genome; full = TRUE copies the whole genome, else
// only coat/shiny/hue/speciation (lethals cleared).
static void MakeMon(struct Pokemon *mon, u16 species, u8 level, bool8 female, const struct Genome *genes, bool8 full)
{
    struct Genome g;
    u32 personality = (Random() << 16) | (Random() & 0xFF00) | (female ? 0x00 : 0xFF);
    CreateMon(mon, species, level, personality, OTID_STRUCT_PLAYER_ID);
    GetBoxMonGenome(&mon->box, &g);
    if (genes && full)
        g = *genes;
    else if (genes)
    {
        memcpy(g.coat, genes->coat, sizeof(g.coat));
        g.shiny = genes->shiny; g.hue[0] = genes->hue[0]; g.hue[1] = genes->hue[1];
        memcpy(g.spec, genes->spec, sizeof(g.spec));
        g.lethal[0] = g.lethal[1] = 0; g.dead = 0;
    }
    SetBoxMonGenome(&mon->box, &g);
    CalculateMonStats(mon);
}
static void ClearDaycare(void)
{
    u8 *p = (u8 *)&gSaveBlock1Ptr->daycare; u32 i;
    for (i = 0; i < sizeof(gSaveBlock1Ptr->daycare); i++) p[i] = 0;
    FlagClear(FLAG_PENDING_DAYCARE_EGG);
}
static void SetDaycare(struct Pokemon *a, struct Pokemon *b)
{
    ClearDaycare();
    gSaveBlock1Ptr->daycare.mons[0].mon = a->box;
    gSaveBlock1Ptr->daycare.mons[1].mon = b->box;
}
static u8 DaycareScore(struct Pokemon *a, struct Pokemon *b)
{
    u8 s;
    SetDaycare(a, b);
    s = GetDaycareCompatibilityScore(&gSaveBlock1Ptr->daycare);
    ClearDaycare();
    return s;
}
static bool8 SameGenome(const struct Genome *a, const struct Genome *b)
{
    u32 pa[GENOME_PACKED_WORDS], pb[GENOME_PACKED_WORDS];
    PackGenome(a, pa); PackGenome(b, pb);
    return pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2];
}
static void ArmHatch(void)
{
    u32 zero = 0; int i;
    for (i = 0; i < gPlayerPartyCount; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            SetMonData(&gPlayerParty[i], MON_DATA_FRIENDSHIP, &zero);
    gSaveBlock1Ptr->daycare.stepCounter = 254;
}
static void TakeStep(void) { sWalkDir ^= 1; Hold(sWalkDir ? DPAD_DOWN : DPAD_UP, 24); }
static void ResetParty(void)
{
    int i;
    for (i = 0; i < PARTY_SIZE; i++) ZeroMonData(&gPlayerParty[i]);
    CalculatePlayerPartyCount();
}
// what hatching gives (egg personality + genome), as a level-20 Pokémon for the next generation
static void HatchCopy(struct Pokemon *dst, struct Pokemon *egg)
{
    struct Genome g;
    GetBoxMonGenome(&egg->box, &g);
    u16 gen = GetMonData(egg, MON_DATA_GENERATION);
    CreateMon(dst, GetMonData(egg, MON_DATA_SPECIES), 20, GetMonData(egg, MON_DATA_PERSONALITY), OTID_STRUCT_PLAYER_ID);
    SetMonData(dst, MON_DATA_GENERATION, &gen);
    SetBoxMonGenome(&dst->box, &g);
    CalculateMonStats(dst);
}
static void SetMoves(struct Pokemon *mon, u16 move)
{
    u32 i;
    for (i = 0; i < MAX_MON_MOVES; i++) SetMonMoveSlot(mon, i == 0 ? move : MOVE_NONE, i);
}
static u32 MonDerived(struct Pokemon *mon)
{
    struct Genome g;
    GetBoxMonGenome(&mon->box, &g);
    return GenomeSpecDerivedCopies(&g);
}
static u32 MonSize(struct Pokemon *mon)
{
    struct Genome g;
    GetBoxMonGenome(&mon->box, &g);
    return GenomeSize(&g, GetMonData(mon, MON_DATA_PERSONALITY));
}
static u32 WildTypeDistance(struct Pokemon *mon)
{
    struct Genome g, zero = {0};
    GetBoxMonGenome(&mon->box, &g);
    return GenomeSpeciesDistance(&g, &zero);
}
static void LogMon(const char *label, struct Pokemon *mon)
{
    struct Genome g; u32 i;
    GetBoxMonGenome(&mon->box, &g);
    LogStr("    "); LogStr(label); LogStr(" spec ");
    for (i = 0; i < GENOME_SPEC_LOCI; i++) { LogNum(g.spec[i]); if (i == 3) LogStr("|"); }
    LogStr(" size "); LogNum(GenomeSize(&g, GetMonData(mon, MON_DATA_PERSONALITY)));
    LogStr(" x256 "); LogNum(GenomeSizeScale(&g, GetMonData(mon, MON_DATA_PERSONALITY)));
    LogStr(" d(wild) "); LogNum(GenomeSpeciesDistance(&g, &(struct Genome){0})); LogStr("\n");
}

enum { ST_BOOT, ST_SETUP, ST_UNIT, ST_REPLICATES, ST_LINE, ST_DAYCARE, ST_EGGS, ST_HATCH, ST_DEAD_EGG, ST_SAVES, ST_FOLLOWER, ST_GRASS, ST_BATTLE, ST_PARTY, ST_EVOLVE, ST_PC, ST_EEVEE_STATS, ST_EEVEE_CAPTURE, ST_CAPTURE_SAVES, ST_HYBRID, ST_CROSSBREED, ST_DONE };

static void Boot(void)
{
    sTimer++;
    if (gMain.callback2 == CB2_Overworld && FieldIdle() && sTimer > 60) { LogStr("[boot] new game on Route 117 by the Day Care\n"); NextStage(); return; }
    if ((sTimer % 60) == 0) Press((sTimer / 60) & 1 ? START_BUTTON : A_BUTTON);
}

static void Setup(void)
{
    int i;
    for (i = 0; i < TRAINERS_COUNT; i++) FlagSet(TRAINER_FLAGS_START + i);
    gSaveBlock2Ptr->optionsTextSpeed = OPTIONS_TEXT_SPEED_FAST;
    VarSet(VAR_REPEL_STEP_COUNT, 250);
    FlagSet(FLAG_SYS_POKEMON_GET); FlagSet(FLAG_SYS_POKEDEX_GET); FlagSet(FLAG_SYS_B_DASH); FlagSet(FLAG_RECEIVED_BIKE);
    AddBagItem(ITEM_MACH_BIKE, 1); AddBagItem(ITEM_POKE_BALL, 20);
    Shot("boot_field");
    NextStage();
    if (gEvoTest.pad == 1) { LogStr("[mode] Eevee script numbers only\n"); sStage = ST_EEVEE_STATS; }
    if (gEvoTest.pad == 2) { LogStr("[mode] Eevee capture line only\n"); sStage = ST_EEVEE_CAPTURE; }
    if (gEvoTest.pad == 3) { LogStr("[mode] hybrid sprites only\n"); sStage = ST_HYBRID; }
    if (gEvoTest.pad == 4) { LogStr("[mode] cross-breeding only\n"); sStage = ST_CROSSBREED; }
}

// ---------------------------------------------------------------- pure logic checks
static void RandomGenome(struct Genome *g)
{
    u32 i, j;
    memset(g, 0, sizeof(*g));
    for (i = 0; i < NUM_STATS; i++) for (j = 0; j < GENOME_IV_LOCI; j++) g->iv[i][j] = Random() & 3;
    for (i = 0; i < NUM_COAT_LOCI; i++) g->coat[i] = Random() & 3;
    g->shiny = Random() & 3;
    g->lethal[0] = Random() % 128;
    g->dead = Random() & 1;
    g->hue[0] = (s8)(Random() % 16) - 8; g->hue[1] = (s8)(Random() % 16) - 8;
    for (i = 0; i < GENOME_SPEC_LOCI; i++) g->spec[i] = Random() & 3;
}

static void Unit(void)
{
    u32 n, bad, k, i;
    struct Genome a, b, z = {0}, c;
    struct Pokemon mon;

    LogStr("[unit] 77-bit genome v3\n");
    // 1. pack / unpack round trip (canonical bits)
    for (n = 0, bad = 0; n < 3000; n++)
    {
        u32 p[GENOME_PACKED_WORDS];
        RandomGenome(&a);
        PackGenome(&a, p);
        if (!UnpackGenome(p, &b) || memcmp(&a, &b, sizeof(a)) != 0 || (p[2] >> (76 - 64)) != 1) bad++;
    }
    Check(bad == 0, "pack/unpack round trip, 3000 random genomes (all 77 bits)");
    // 2. through the BoxPokemon fields: genome survives, checksum valid, other data untouched
    for (n = 0, bad = 0; n < 300; n++)
    {
        u32 pid, ot, exp; u8 nick[POKEMON_NAME_LENGTH + 1], nick2[POKEMON_NAME_LENGTH + 1];
        MakeMon(&mon, SPECIES_EEVEE, 5 + (n % 50), n & 1, NULL, FALSE);
        pid = GetMonData(&mon, MON_DATA_PERSONALITY); ot = GetMonData(&mon, MON_DATA_OT_ID); exp = GetMonData(&mon, MON_DATA_EXP);
        GetMonData(&mon, MON_DATA_NICKNAME, nick);
        RandomGenome(&a);
        SetBoxMonGenome(&mon.box, &a);
        GetBoxMonGenome(&mon.box, &b);
        GetMonData(&mon, MON_DATA_NICKNAME, nick2);
        if (memcmp(&a, &b, sizeof(a)) != 0 || GetMonData(&mon, MON_DATA_SANITY_IS_BAD_EGG)
         || GetMonData(&mon, MON_DATA_PERSONALITY) != pid || GetMonData(&mon, MON_DATA_OT_ID) != ot || GetMonData(&mon, MON_DATA_EXP) != exp
         || GetMonData(&mon, MON_DATA_SPECIES) != SPECIES_EEVEE || memcmp(nick, nick2, sizeof(nick)) != 0
         || GetMonData(&mon, MON_DATA_HP_IV) != GenomeIV(&a, 0) || GetMonData(&mon, MON_DATA_SPDEF_IV) != GenomeIV(&a, 5)
         || (bool32)GetMonData(&mon, MON_DATA_IS_SHINY) != (bool32)GenomeIsShiny(&a))
            bad++;
    }
    Check(bad == 0, "genome round trip through BoxPokemon fields, 300 mons (checksum, PID, OT, EXP, name, IVs, shiny intact)");
    // 3. wild genomes: B never derived; any two wild Pokémon are fully compatible
    for (n = 0, bad = 0, k = 0; n < 10000; n++)
    {
        GenerateWildGenome(&a); GenerateWildGenome(&b);
        for (i = 0; i < GENOME_SPEC_PAIRS; i++) if (a.spec[GENOME_SPEC_PAIRS + i]) bad++;
        if (GenomeCompatibilityScore(&a, &b) != 70) k++;
    }
    Check(bad == 0, "wild genomes never carry a derived B allele");
    Check(k == 0, "10000 random wild pairs all score 70 (\"get along very well\")");
    // 4. mutation: 16 copies x 1/64 per egg ~ 0.25 derived copies per egg from wild-type parents
    for (n = 0, k = 0, i = 0; n < 2000; n++) { InheritGenome(&c, &z, &z); k += GenomeSpecDerivedCopies(&c); if (c.hue[0] || c.hue[1]) i++; }
    LogStr("  2000 eggs from wild-type parents: new derived copies "); LogNum(k); LogStr(" (expect ~500), hue mutants "); LogNum(i); LogStr(" (expect ~167)\n");
    Check(k > 420 && k < 580, "speciation-locus mutation rate 1/64 per copy");
    Check(i > 130 && i < 205, "hue mutation rate ~2/24 per egg");
    // 5. hybrid inviability 2^-n: parents fixed for derived B next to ancestral A at 1 / 2 / 4 pairs
    for (k = 1; k <= 4; k <<= 1)
    {
        u32 dead = 0;
        memset(&a, 0, sizeof(a));
        for (i = 0; i < k; i++) a.spec[GENOME_SPEC_PAIRS + i] = 3;
        for (n = 0; n < 2000; n++) { InheritGenome(&c, &a, &a); if (c.dead) dead++; }
        LogStr("  n="); LogNum(k); LogStr(": "); LogNum(dead); LogStr("/2000 eggs inviable (expect ");
        LogNum(2000 - 2000 / (1 << k)); LogStr(", less a few from new A mutations)\n");
        if (k == 1) Check(dead > 880 && dead < 1120, "one incompatible pair: about half the eggs die (2^-1)");
        if (k == 2) Check(dead > 1400 && dead < 1600, "two incompatible pairs: about 3/4 die (2^-2)");
        if (k == 4) Check(dead > 1800, "four incompatible pairs: about 15/16 die (2^-4)");
    }
    // 6. score table on hand-built genomes
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    for (i = 0; i < 4; i++) { a.spec[i] = 3; a.spec[GENOME_SPEC_PAIRS + i] = 3; }
    Check(GenomeCompatibilityScore(&a, &b) == 0, "finished line (AABB at all 4 pairs) vs wild-type scores 0");
    a.spec[GENOME_SPEC_PAIRS + 3] = 1;
    Check(GenomeCompatibilityScore(&a, &b) == 0, "3 completed pairs still score 0");
    a.spec[GENOME_SPEC_PAIRS + 2] = 1;
    Check(GenomeCompatibilityScore(&a, &b) == 20, "2 completed pairs score 20");
    a.spec[GENOME_SPEC_PAIRS + 1] = 0;
    Check(GenomeCompatibilityScore(&a, &b) == 50, "1 completed pair scores 50");
    b.spec[0] = 1;
    Check(GenomeCompatibilityScore(&a, &b) == 70, "a wild partner with one derived A copy blocks that pair: 70");
    NextStage();
}

// Mirrors speciation_sim.py's 'size' strategy with the ROM's own InheritGenome: one line per frame.
static void Replicates(void)
{
    struct Genome mum, dad, kid, bestF, bestM, zero = {0};
    u32 gen, e, sizeF, sizeM, s;
    bool8 haveF, haveM;

    bool8 eevee = sSub != 0;   // second pass: females 1 in 8 (Eevee's gender ratio) instead of 1 in 2
    if (sCount == 0) LogStr(eevee ? "[replicates] same, but 1 egg in 8 female (Eevee)\n" : "[replicates] 200 size-selected sib lines, 10 eggs/gen, ROM genetics\n");
    GenerateWildGenome(&mum); GenerateWildGenome(&dad);
    for (gen = 1; gen <= LINE_MAX_GENS; gen++)
    {
        haveF = haveM = FALSE; sizeF = sizeM = 0;
        for (e = 0; e < LINE_EGGS; e++)
        {
            InheritGenome(&kid, &mum, &dad);
            if (kid.dead) continue;
            s = GenomeSize(&kid, Random()) * 2 + (Random() & 1);    // size + tiebreak
            if (eevee ? (Random() & 7) == 0 : (Random() & 1)) { if (!haveF || s > sizeF) { bestF = kid; sizeF = s; haveF = TRUE; } }
            else              { if (!haveM || s > sizeM) { bestM = kid; sizeM = s; haveM = TRUE; } }
        }
        if (haveF) mum = bestF;
        if (haveM) dad = bestM;
        if (GenomeSpeciesDistance(&mum, &zero) >= 3 && GenomeSpeciesDistance(&dad, &zero) >= 3)
            break;
    }
    sRepGens[sCount] = gen > LINE_MAX_GENS ? 255 : gen;
    if (++sCount >= REP_LINES)
    {
        u32 i, j, t;
        for (i = 1; i < REP_LINES; i++) for (j = i; j > 0 && sRepGens[j - 1] > sRepGens[j]; j--) { t = sRepGens[j]; sRepGens[j] = sRepGens[j - 1]; sRepGens[j - 1] = t; }
        LogStr("  generations to score 0 vs wild-type, p10/median/p90: ");
        LogNum(sRepGens[REP_LINES / 10]); LogStr(" / "); LogNum(sRepGens[REP_LINES / 2]); LogStr(" / "); LogNum(sRepGens[REP_LINES * 9 / 10]);
        LogStr(eevee ? "   (no sim; 255 = not reached in 60)\n" : "   (sim: 14 / 20 / 29)\n");
        if (!eevee)
        {
            Check(sRepGens[REP_LINES / 2] >= 16 && sRepGens[REP_LINES / 2] <= 25, "ROM median generations to a new species matches the sim (16-25)");
            sSub = 1; sCount = 0;
        }
        else
            NextStage();
    }
}

// The real Day Care path (TriggerPendingDaycareEgg + GiveEggFromDaycare), one generation per frame: a selected
// Eevee sib line from two wild catches, keeping the biggest son and daughter of 10 eggs.
static void Line(void)
{
    struct Pokemon bestF, bestM, *egg;
    u32 e, s, sizeF = 0, sizeM = 0;
    bool8 haveF = FALSE, haveM = FALSE;
    struct Genome g;

    if (sLineGen == LINE_WANT_GENS && sLineAttempt < 15 && !(WildTypeDistance(&sMum) >= 3 && WildTypeDistance(&sDad) >= 3))
    {
        LogStr("  line attempt "); LogNum(sLineAttempt + 1); LogStr(" not finished by gen 24; new wild founders\n");
        sLineAttempt++; sLineGen = 0; sLineEggs = 0; sLineFailed = 0; sLineStalls = 0;
    }
    if (sLineGen == 0)
    {
        LogStr("[line] Eevee sib line through the real Day Care, biggest son + daughter of 10 eggs\n");
        ResetParty();
        MakeMon(&gPlayerParty[0], SPECIES_MUDKIP, 10, FALSE, NULL, FALSE);
        CalculatePlayerPartyCount();
        MakeMon(&sMum, SPECIES_EEVEE, 20, TRUE, NULL, FALSE);
        MakeMon(&sDad, SPECIES_EEVEE, 20, FALSE, NULL, FALSE);
        do MakeMon(&sWildEevee, SPECIES_EEVEE, 20, FALSE, NULL, FALSE); while (MonDerived(&sWildEevee) != 0);  // wild male, no derived A copy (95% of catches)
        do MakeMon(&sWildF, SPECIES_EEVEE, 20, TRUE, NULL, FALSE); while (MonDerived(&sWildF) != 0);            // wild female, no derived A copy
        LogMon("founder F", &sMum); LogMon("founder M", &sDad);
        Check(DaycareScore(&sMum, &sDad) == 70, "two wild Eevee founders score 70 in the real Day Care");
    }
    sLineGen++;
    SetDaycare(&sMum, &sDad);
    if (GetDaycareCompatibilityScore(&gSaveBlock1Ptr->daycare) != 70)
    {
        LogStr("    gen "); LogNum(sLineGen); LogStr(": parents no longer 70 with each other\n");
    }
    for (e = 0; e < LINE_EGGS; e++)
    {
        TriggerPendingDaycareEgg();
        GiveEggFromDaycare();
        if (gPlayerPartyCount < 2) { Check(FALSE, "Day Care gave no egg"); NextStage(); return; }
        egg = &gPlayerParty[gPlayerPartyCount - 1];
        GetBoxMonGenome(&egg->box, &g);
        sLineEggs++;
        if (g.dead) { sLineFailed++; ZeroMonData(egg); CalculatePlayerPartyCount(); continue; }
        s = MonSize(egg) * 2 + (Random() & 1);   // size reads off the egg's genes + personality, as on hatching
        if (GetMonGender(egg) == MON_FEMALE) { if (!haveF || s > sizeF) { HatchCopy(&bestF, egg); sizeF = s; haveF = TRUE; } }
        else                                  { if (!haveM || s > sizeM) { HatchCopy(&bestM, egg); sizeM = s; haveM = TRUE; } }
        ZeroMonData(egg); CalculatePlayerPartyCount();
    }
    ClearDaycare();
    if (haveF) sMum = bestF;
    if (haveM) sDad = bestM;
    if (!haveF && !haveM) sLineStalls++;
    if (sLineGen == 10) { sGen10[0] = sMum; sGen10[1] = sDad; }
    if (sLineGen % 5 == 0 || (WildTypeDistance(&sMum) >= 3 && WildTypeDistance(&sDad) >= 3))
    {
        LogStr("    gen "); LogNum(sLineGen); LogStr(": score vs wild-type ");
        LogNum(DaycareScore(&sMum, &sWildEevee)); LogStr(" (F) "); LogNum(DaycareScore(&sWildF, &sDad)); LogStr(" (M), within the line "); LogNum(DaycareScore(&sMum, &sDad)); LogStr(", eggs failed so far ");
        LogNum(sLineFailed); LogStr("/"); LogNum(sLineEggs); LogStr("\n");
    }
    if ((WildTypeDistance(&sMum) >= 3 && WildTypeDistance(&sDad) >= 3) || sLineGen >= LINE_MAX_GENS)
    {
        sLineDoneGen = sLineGen;
        sFinal[0] = sMum; sFinal[1] = sDad;
        if (sLineGen < 10) { sGen10[0] = sMum; sGen10[1] = sDad; }
        LogMon("final F", &sMum); LogMon("final M", &sDad);
        LogStr("  new species after "); LogNum(sLineGen); LogStr(" generations, "); LogNum(sLineEggs); LogStr(" eggs\n");
        Check(sLineGen < LINE_MAX_GENS, "selected line reached score 0 against wild-type (both kept parents)");
        LogStr("  generation counter: F "); LogNum(GetMonData(&sMum, MON_DATA_GENERATION)); LogStr(", M "); LogNum(GetMonData(&sDad, MON_DATA_GENERATION)); LogStr("\n");
        {
            u32 gf = GetMonData(&sMum, MON_DATA_GENERATION), gm = GetMonData(&sDad, MON_DATA_GENERATION);
            LogStr("  generations where every egg failed: "); LogNum(sLineStalls); LogStr("\n");
            Check((gf > gm ? gf : gm) + sLineStalls == sLineGen && gf + sLineStalls + 5 >= sLineGen && gm + sLineStalls + 5 >= sLineGen,
                  "generation counter = generations bred (wild founders = 0; a generation with no surviving egg adds none)");
        }
        {
            u8 name[POKEMON_NAME_LENGTH + 1];
            static const u8 sLong[] = _("ABCDEFGHIJKL");
            u16 gen = GetMonData(&sMum, MON_DATA_GENERATION);
            SetMonData(&sMum, MON_DATA_NICKNAME, sLong);
            GetMonData(&sMum, MON_DATA_NICKNAME, name);
            Check(GetMonData(&sMum, MON_DATA_GENERATION) == gen && StringLength(name) == 10, "a 12-char nickname is cut to 10 and keeps the generation");
            SetMonData(&sMum, MON_DATA_NICKNAME, GetSpeciesName(SPECIES_EEVEE));
        }
        Check(DaycareScore(&sMum, &sWildEevee) == 0 && DaycareScore(&sWildF, &sDad) == 0, "real Day Care: line x wild Eevee (either sex) scores 0");
        LogStr("  the line with itself scores "); LogNum(DaycareScore(&sMum, &sDad)); LogStr("\n");
        Check(DaycareScore(&sMum, &sDad) > 0, "real Day Care: the line still breeds with itself");
        NextStage();
    }
}

// Day Care man reads the compatibility line from the real script
static void DaycareMan(void)
{
    static const char *const names[] = {"daycare_wild_pair", "daycare_gen10_vs_wild", "daycare_new_species_vs_wild"};
    switch (sSub)
    {
    case 0:
    case 3:
    case 6:
        if (!FieldIdle()) return;
        if (sSub == 0) SetDaycare(&sWildF, &sWildEevee);
        if (sSub == 3) SetDaycare(&sGen10[0], &sWildEevee);
        if (sSub == 6) SetDaycare(&sFinal[0], &sWildEevee);
        LogStr("  Day Care score "); LogNum(GetDaycareCompatibilityScore(&gSaveBlock1Ptr->daycare)); LogStr("\n");
        ScriptContext_SetupScript(Route117_EventScript_CheckOnTwoMons);
        sTimer = 0; sSub++;
        break;
    case 1:
    case 4:
    case 7:
        sTimer++;
        if (sTimer == 60) Press(A_BUTTON);                // "Your EEVEE and EEVEE are doing fine."
        if (sTimer == 150) Shot(names[sSub / 3]);         // the compatibility line
        if (sTimer > 160 && (sTimer % 40) == 0) Press(A_BUTTON);
        if (sTimer > 200 && FieldIdle()) { sTimer = 0; sSub++; }
        if (sTimer > 1200) { Check(FALSE, "Day Care man script did not finish"); sSub++; }
        break;
    case 2:
    case 5:
        sSub++;
        break;
    default:
        Check(gSpecialVar_Result == 0, "Day Care man: new species x wild Eevee -> \"prefer to play with other POKeMON\"");
        ClearDaycare();
        NextStage();
        break;
    }
}

// Day Care eggs through the game's own path (TriggerPendingDaycareEgg + GiveEggFromDaycare)
static struct Pokemon *DaycareEggOf(u16 mumSpecies, u16 dadSpecies, const struct Genome *mum, const struct Genome *dad, bool8 full)
{
    struct Pokemon a, b;
    MakeMon(&a, mumSpecies, 20, TRUE, mum, full);
    MakeMon(&b, dadSpecies, 20, FALSE, dad, full);
    SetDaycare(&a, &b);
    TriggerPendingDaycareEgg();
    GiveEggFromDaycare();
    return &gPlayerParty[gPlayerPartyCount - 1];
}
static struct Pokemon *DaycareEgg(const struct Genome *mum, const struct Genome *dad) { return DaycareEggOf(SPECIES_PIKACHU, SPECIES_PIKACHU, mum, dad, FALSE); }

static void Eggs(void)
{
    struct Genome albino = {0}, hueA = {0}, hueB = {0}, wild = {0}, broken = {0};
    struct Genome g, pg; int i, tries;
    struct Pokemon *egg, a, b;

    LogStr("[eggs] real Day Care path\n");
    albino.coat[COAT_LOCUS_C] = 3;
    hueA.hue[0] = 5; hueA.hue[1] = 4;
    hueB.hue[0] = 4; hueB.hue[1] = 5;
    ResetParty();
    MakeMon(&gPlayerParty[0], SPECIES_MUDKIP, 10, FALSE, NULL, FALSE);
    CalculatePlayerPartyCount();

    // family barrier + Ditto clone (realgen ports)
    MakeMon(&a, SPECIES_PIKACHU, 20, TRUE, NULL, FALSE); MakeMon(&b, SPECIES_EEVEE, 20, FALSE, NULL, FALSE);
    // crossbreed (GAME_SPEC feature 2) replaced the family barrier: two species make a hybrid egg (score 20)
    Check(DaycareScore(&a, &b) == PARENTS_LOW_COMPATIBILITY, "cross-species: Pikachu x Eevee can now make a (hybrid) egg");
    MakeMon(&b, SPECIES_RAICHU, 20, FALSE, NULL, FALSE);
    Check(DaycareScore(&a, &b) == 70, "same family (Pikachu x Raichu) breeds");
    for (tries = 0, i = 0; tries < 8; tries++)
    {
        MakeMon(&a, SPECIES_DITTO, 20, FALSE, NULL, FALSE);
        MakeMon(&b, (tries & 1) ? SPECIES_NIDORAN_M : SPECIES_PIKACHU, 20, FALSE, NULL, FALSE);
        SetDaycare(&a, &b);
        TriggerPendingDaycareEgg(); GiveEggFromDaycare();
        egg = &gPlayerParty[gPlayerPartyCount - 1];
        GetBoxMonGenome(&egg->box, &g); GetBoxMonGenome(&b.box, &pg);
        if (!GetMonData(egg, MON_DATA_IS_EGG) || GetMonGender(egg) != MON_MALE || !SameGenome(&g, &pg)
         || GetMonData(egg, MON_DATA_ABILITY_NUM) != GetMonData(&b, MON_DATA_ABILITY_NUM)
         || GetMonData(egg, MON_DATA_SPECIES) != ((tries & 1) ? SPECIES_NIDORAN_M : SPECIES_PICHU))
            i++;
        ZeroMonData(egg); CalculatePlayerPartyCount();
    }
    Check(i == 0, "Ditto x male: egg is a male clone (same genome, ability slot, Nidoran-M stays Nidoran-M), 8/8");
    ClearDaycare();

    egg = DaycareEgg(&albino, &albino);
    GetBoxMonGenome(&egg->box, &g);
    Check(GetMonData(egg, MON_DATA_IS_EGG) && GetMonData(egg, MON_DATA_SPECIES) == SPECIES_PICHU, "Day Care made a Pichu egg");
    Check(GenomeCoat(&g) == COAT_ALBINO, "albino x albino egg is albino (c/c)");
    for (tries = 0; tries < 50; tries++)
    {
        egg = DaycareEgg(&hueA, &hueB);
        GetBoxMonGenome(&egg->box, &g);
        if (g.hue[0] + g.hue[1] >= 9 && !g.dead) break;
        ZeroMonData(egg); CalculatePlayerPartyCount();
    }
    Check(g.hue[0] >= 3 && g.hue[1] >= 3, "hue alleles inherited from both parents (+/-1 mutation)");
    for (tries = 0; tries < 20; tries++)
    {
        egg = DaycareEgg(&wild, &wild);
        GetBoxMonGenome(&egg->box, &g);
        if (!g.dead) break;
        ZeroMonData(egg); CalculatePlayerPartyCount();
    }
    for (i = 1; i < gPlayerPartyCount; i++) GetBoxMonGenome(&gPlayerParty[i].box, &sEggGenome[i]);
    Check(gPlayerPartyCount == 4, "party = Mudkip + 3 eggs");

    // a hybrid: mother fixed for derived B1 (aa at A1), father wild-type -> score 50, and every egg is B1 next to a1
    broken.spec[GENOME_SPEC_PAIRS + 0] = 3;
    memset(&g, 0, sizeof(g));
    for (tries = 0; tries < 40; tries++)
    {
        u8 before = gPlayerPartyCount;
        egg = DaycareEggOf(SPECIES_EEVEE, SPECIES_EEVEE, &broken, &wild, FALSE);
        if (gPlayerPartyCount == before) { Check(FALSE, "hybrid pair laid no egg"); break; }
        GetBoxMonGenome(&egg->box, &g);
        sDeadEgg = *egg;
        ZeroMonData(egg); CalculatePlayerPartyCount();
        if (g.dead) break;
    }
    LogStr("  hybrid egg after "); LogNum(tries + 1); LogStr(" eggs, incompatible pairs "); LogNum(GenomeIncompatiblePairs(&g)); LogStr("\n");
    Check(g.dead && GenomeIncompatiblePairs(&g) >= 1, "hybrid egg (incompatible pairs) marked inviable at laying");
    ClearDaycare();
    sCount = 1;
    NextStage();
}

static void Hatch(void)
{
    u8 state; struct Genome g;
    static const char *const names[] = {"", "hatch_albino", "hatch_hue_mutant", "hatch_wildtype"};
    switch (sSub)
    {
    case 0: if (sCount == 0) sCount = 1; ArmHatch(); TakeStep(); sTimer = 0; sLastHatchState = 0xFF; sSub++; break;
    case 1:
        state = EvoTest_EggHatchState();
        if (state == 6 && sLastHatchState != 6) sStateTicks = 0;
        sLastHatchState = state;
        if (state == 6 && ++sStateTicks == 50) Shot(names[sCount]);
        if (state == 0xFF || state >= 9)
        {
            if (MashBUntilIdle())
            {
                GetBoxMonGenome(&gPlayerParty[sCount].box, &g);
                Check(!GetMonData(&gPlayerParty[sCount], MON_DATA_IS_EGG) && GetMonData(&gPlayerParty[sCount], MON_DATA_SPECIES) == SPECIES_PICHU, "egg hatched into a Pichu");
                Check(SameGenome(&g, &sEggGenome[sCount]), "hatchling has exactly the egg's genome");
                sTimer = 0;
                if (++sCount >= gPlayerPartyCount) NextStage(); else sSub = 0;
            }
        }
        else if (++sTimer > 60 * 60) { Check(FALSE, "hatch did not finish"); NextStage(); }
        break;
    }
}

// the inviable egg goes through EventScript_EggHatch: "The EGG didn't hatch..."
static void DeadEggHatch(void)
{
    switch (sSub)
    {
    case 0:
        gPlayerParty[gPlayerPartyCount] = sDeadEgg;
        CalculatePlayerPartyCount();
        sCount = gPlayerPartyCount;
        ArmHatch(); TakeStep(); sTimer = 0; sSub++;
        break;
    case 1:
        if (ArePlayerFieldControlsLocked() || sTimer > 0) sTimer++;
        if (sTimer == 50) Shot("dead_egg_huh");
        if (sTimer == 60 || sTimer == 150 || sTimer == 240 || sTimer == 330) Press(A_BUTTON);
        if (sTimer == 140) Shot("dead_egg_dots");
        if (sTimer == 230) Shot("dead_egg_didnt_hatch");
        if (sTimer == 320) Shot("dead_egg_no_longer_moving");
        if (sTimer > 340 && FieldIdle())
        {
            u32 i, eggs = 0;
            for (i = 0; i < gPlayerPartyCount; i++) if (GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG)) eggs++;
            Check(gPlayerPartyCount == sCount - 1 && eggs == 0, "inviable egg removed from the party after \"The EGG didn't hatch\"");
            NextStage();
        }
        if (sTimer > 60 * 60) { Check(FALSE, "dead egg script did not finish"); NextStage(); }
        break;
    }
}

// reusable saves for scripted captures (copied to C:\Users\ivank\Documents\poke-evolution\saves by rom_test.sh)
static void Saves(void)
{
    if (sTimer++ < 20) return;   // let the runner dump the previous one
    sTimer = 0;
    ResetParty(); ClearDaycare();
    switch (sSub++)
    {
    case 0:
        LogStr("[saves]\n");
        MakeMon(&gPlayerParty[0], SPECIES_EEVEE, 20, TRUE, NULL, FALSE);
        MakeMon(&gPlayerParty[1], SPECIES_EEVEE, 20, FALSE, NULL, FALSE);
        CalculatePlayerPartyCount();
        TrySavingData(SAVE_NORMAL); DumpSave("01_wild_pair.sav");
        break;
    case 1:
        gPlayerParty[0] = sGen10[0]; gPlayerParty[1] = sGen10[1];
        CalculatePlayerPartyCount();
        TrySavingData(SAVE_NORMAL); DumpSave("02_gen10_pair.sav");
        break;
    case 2:
        gPlayerParty[0] = sFinal[0]; gPlayerParty[1] = sFinal[1]; gPlayerParty[2] = sWildEevee;
        CalculatePlayerPartyCount();
        TrySavingData(SAVE_NORMAL); DumpSave("03_new_species_plus_wild.sav");
        break;
    default:
        NextStage();
        break;
    }
}

// Follower: a big (finished-line genes) hue-mutant Eevee leads; walk all four directions (east uses h-flip)
static void Follower(void)
{
    static const u16 dirs[] = {DPAD_DOWN, DPAD_DOWN, DPAD_LEFT, DPAD_LEFT, DPAD_UP, DPAD_UP, DPAD_RIGHT, DPAD_RIGHT};
    static const char *const names[] = {"follower_down", "follower_left", "follower_up", "follower_right"};
    struct Genome g;
    sTimer++;
    if (sTimer == 1)
    {
        ResetParty();
        GetBoxMonGenome(&sFinal[1].box, &g);
        g.hue[0] = 6; g.hue[1] = 6;
        MakeMon(&gPlayerParty[0], SPECIES_EEVEE, 20, FALSE, &g, TRUE);
        gPlayerParty[1] = sWildEevee;
        CalculatePlayerPartyCount();
        LogMon("follower", &gPlayerParty[0]);
        UpdateFollowingPokemon();
    }
    if (sTimer >= 10 && sTimer < 10 + 8 * 40 && ((sTimer - 10) % 40) == 0)
        Hold(dirs[(sTimer - 10) / 40], 16);
    if (sTimer >= 10 && sTimer < 10 + 8 * 40 && ((sTimer - 10) % 80) == 70)
    {
        struct ObjectEvent *f = GetFollowerObject();
        LogStr("  follower display scale x256: "); LogNum(f ? GetSpriteDisplayScale(f->spriteId) : -1);
        LogStr(", palette "); LogNum(f ? gSprites[f->spriteId].oam.paletteNum : -1); LogStr("\n");
        Shot(names[(sTimer - 10) / 80]);
    }
    if (sTimer == 10 + 8 * 40 + 20)
    {
        // swap in the wild-type Eevee to compare (normal colours, ~1.0x)
        struct Pokemon tmp = gPlayerParty[0]; gPlayerParty[0] = gPlayerParty[1]; gPlayerParty[1] = tmp;
        UpdateFollowingPokemon();
    }
    if (sTimer == 10 + 8 * 40 + 40) Hold(DPAD_DOWN, 16);
    if (sTimer == 10 + 8 * 40 + 80) Hold(DPAD_RIGHT, 16);
    if (sTimer == 10 + 8 * 40 + 120)
    {
        struct ObjectEvent *f = GetFollowerObject();
        LogStr("  wild-type follower display scale x256: "); LogNum(f ? GetSpriteDisplayScale(f->spriteId) : -1); LogStr("\n");
        Shot("follower_wildtype_right");
    }
    if (sTimer == 10 + 8 * 40 + 130) NextStage();
}

// The 1.5x follower walks through tall grass: find a 3-tile vertical strip of tall grass near the player, warp to its
// top and walk down two steps, so the follower stands on grass.
static void Grass(void)
{
    static EWRAM_DATA s16 gx = 0, gy = 0;
    struct Genome g;
    sTimer++;
    if (sTimer == 1)
    {
        struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
        s32 x, y, r, found = 0;
        ResetParty();
        GetBoxMonGenome(&sFinal[1].box, &g);
        g.hue[0] = 6; g.hue[1] = 6;
        MakeMon(&gPlayerParty[0], SPECIES_EEVEE, 20, FALSE, &g, TRUE);
        CalculatePlayerPartyCount();
        for (r = 1; r < 40 && !found; r++)
            for (y = pl->currentCoords.y - r; y <= pl->currentCoords.y + r && !found; y++)
                for (x = pl->currentCoords.x - r; x <= pl->currentCoords.x + r && !found; x++)
                    if (MetatileBehavior_IsTallGrass(MapGridGetMetatileBehaviorAt(x, y))
                     && MetatileBehavior_IsTallGrass(MapGridGetMetatileBehaviorAt(x, y + 1))
                     && MetatileBehavior_IsTallGrass(MapGridGetMetatileBehaviorAt(x, y + 2)))
                        found = 1, gx = x, gy = y;
        if (!found) { LogStr("  no tall grass strip nearby\n"); NextStage(); return; }
        LogStr("  tall grass strip at "); LogNum(gx - MAP_OFFSET); LogStr(","); LogNum(gy - MAP_OFFSET); LogStr("\n");
        SetWarpDestination(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum, WARP_ID_NONE, gx - MAP_OFFSET, gy - MAP_OFFSET);
        DoWarp();
    }
    if (sTimer > 1 && sTimer < 400 && !FieldIdle()) return;
    if (sTimer < 400) { sTimer = 400; return; }
    if (sTimer == 410) Hold(DPAD_DOWN, 16);
    if (sTimer == 450) Hold(DPAD_DOWN, 16);
    if (sTimer == 520)
    {
        struct ObjectEvent *f = GetFollowerObject();
        LogStr("  follower in grass: display scale x256 "); LogNum(f ? GetSpriteDisplayScale(f->spriteId) : -1); LogStr("\n");
        Shot("follower_1.5x_in_tall_grass");
    }
    if (sTimer == 530) NextStage();
}

// Size made visible: the line's Eevee (gen 10) vs a finished-line Eevee, through a full turn of move animations;
// then flee and fight a wild-type Eevee with the finished-line one.
static void Battle(void)
{
    u32 t;
    switch (sSub)
    {
    case 0:
    case 10:
        ResetParty();
        gPlayerParty[0] = sSub == 0 ? sGen10[1] : sFinal[1];
        CalculatePlayerPartyCount();
        ZeroEnemyPartyMons();
        gEnemyParty[0] = sSub == 0 ? sFinal[0] : sWildEevee;
        if (sSub == 0) { SetMoves(&gPlayerParty[0], MOVE_DEFENSE_CURL); SetMoves(&gEnemyParty[0], MOVE_MINIMIZE); }
        LogMon(sSub == 0 ? "battle 1 player" : "battle 2 player", &gPlayerParty[0]);
        LogMon(sSub == 0 ? "battle 1 enemy " : "battle 2 enemy ", &gEnemyParty[0]);
        BattleSetup_StartWildBattle();
        sTimer = 0; sSub++;
        break;
    case 1:
    case 11:
        sTimer++;
        if (sTimer == 60 * 6)   // v1.0.1: the opponent must never Dynamax or Terastallize (1.0.0: every trainer could)
        {
            u32 opp = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
            LogStr("  opponent gimmicks: dynamax "); LogNum(ShouldTrainerBattlerUseGimmick(opp, GIMMICK_DYNAMAX));
            LogStr(" tera "); LogNum(ShouldTrainerBattlerUseGimmick(opp, GIMMICK_TERA));
            LogStr(" canDynamax "); LogNum(CanDynamax(opp)); LogStr(" canTera "); LogNum(CanTerastallize(opp)); LogStr("\n");
            Check(!ShouldTrainerBattlerUseGimmick(opp, GIMMICK_DYNAMAX) && !ShouldTrainerBattlerUseGimmick(opp, GIMMICK_TERA)
                  && !CanDynamax(opp) && !CanTerastallize(opp), "opponent can't Dynamax or Terastallize");
        }
        if (sTimer < 60 * 12 && (sTimer % 90) == 0) Press(B_BUTTON);
        if (sTimer < 60 * 12) break;
        t = sTimer - 60 * 12;
        if (t == 0) Shot(sSub == 1 ? "battle1_menu" : "battle2_menu");
        if (sSub == 11)
        {
            u32 k = t % 240;
            if (k == 10) Press(DPAD_RIGHT);
            if (k == 20) Press(DPAD_DOWN);
            if (k == 30) Press(A_BUTTON);       // RUN (retry)
            if (k > 40 && (k % 50) == 0) Press(B_BUTTON);
            if (FieldIdle() && t > 100) NextStage();
            if (t > 240 * 20) { Check(FALSE, "could not run from battle 2"); NextStage(); }
            break;
        }
        if (t == 10) Press(A_BUTTON);           // FIGHT
        if (t == 40) Press(A_BUTTON);           // first move
        {
            static const u16 at[] = {70, 90, 110, 130, 150, 250, 280, 310, 340, 370, 400, 430};
            u32 i;
            for (i = 0; i < ARRAY_COUNT(at); i++)
                if (t == at[i]) { char n[] = "battle1_anim_00"; n[13] = '0' + i / 10; n[14] = '0' + i % 10; Shot(n); }
        }
        if (t > 450 && t < 900 && (t % 60) == 0) Press(B_BUTTON);
        if (t == 900) Shot("battle1_after_turn");
        if (t >= 910)   // RUN (retry: escaping a wild battle can fail)
        {
            u32 k = (t - 910) % 240;
            if (k == 0) Press(DPAD_RIGHT);
            if (k == 10) Press(DPAD_DOWN);
            if (k == 20) Press(A_BUTTON);
            if (k > 30 && (k % 50) == 0) Press(B_BUTTON);
            if (FieldIdle() && t > 1000) { sSub = 10; sTimer = 0; }
            if (t > 910 + 240 * 20) { Check(FALSE, "could not run from battle 1"); NextStage(); }
        }
        break;
    }
}

// Genetic colours on the party menu, summary and PC box (icons get their own palettes)
static void ColourMon(struct Pokemon *mon, u16 species, s8 hue, u8 coatLocus)
{
    struct Genome g = {0};
    g.hue[0] = hue / 2; g.hue[1] = hue - hue / 2;
    if (coatLocus < NUM_COAT_LOCI) g.coat[coatLocus] = 3;
    MakeMon(mon, species, 20, Random() & 1, &g, FALSE);
}
static void Party(void)
{
    sTimer++;
    if (sTimer == 1)
    {
        ResetParty();
        ColourMon(&gPlayerParty[0], SPECIES_EEVEE, 12, NUM_COAT_LOCI);      // +135 degrees
        ColourMon(&gPlayerParty[1], SPECIES_EEVEE, 0, COAT_LOCUS_C);        // albino
        ColourMon(&gPlayerParty[2], SPECIES_PIKACHU, -12, NUM_COAT_LOCI);   // -135 degrees
        ColourMon(&gPlayerParty[3], SPECIES_EEVEE, 0, COAT_LOCUS_E);        // red
        ColourMon(&gPlayerParty[4], SPECIES_RALTS, 0, COAT_LOCUS_A);        // black, dual type (HEIGHT layout)
        gPlayerParty[5] = sFinal[0];
        CalculatePlayerPartyCount();
    }
    if (sTimer == 10) Press(START_BUTTON);
    if (sTimer == 40) Press(DPAD_DOWN);
    if (sTimer == 60) Press(A_BUTTON);       // POKeMON
    if (sTimer == 160) Shot("party_menu_genetic_icons");
    if (sTimer == 170) Press(A_BUTTON);      // first mon
    if (sTimer == 200) Press(A_BUTTON);      // SUMMARY
    if (sTimer == 330) Shot("summary_hue_eevee");
    if (sTimer == 340) Press(DPAD_DOWN);     // next mon: albino
    if (sTimer == 460) Shot("summary_albino_eevee");
    if (sTimer == 470) Press(DPAD_DOWN);
    if (sTimer == 590) Shot("summary_hue_pikachu");
    if (sTimer == 600) Press(DPAD_DOWN);
    if (sTimer == 720) Shot("summary_red_eevee");
    if (sTimer == 730) Press(DPAD_DOWN);
    if (sTimer == 850) Shot("summary_black_ralts_dual_type");
    if (sTimer == 860) Press(DPAD_DOWN);
    if (sTimer == 980) Shot("summary_new_species_gen");
    if (sTimer == 990) Press(B_BUTTON);
    if (sTimer == 1060) Press(B_BUTTON);
    if (sTimer == 1120) Press(B_BUTTON);
    if (sTimer > 1160 && MashBUntilIdle()) NextStage();
}
// The evolution scene draws both forms in the genetic colours (species + personality palette hook)
static void Evolve(void)
{
    static EWRAM_DATA struct Genome before = {0};
    struct Genome after;
    sTimer++;
    if (sTimer == 1)
    {
        ResetParty();
        ColourMon(&gPlayerParty[0], SPECIES_PIKACHU, -12, NUM_COAT_LOCI);
        CalculatePlayerPartyCount();
        GetBoxMonGenome(&gPlayerParty[0].box, &before);
        gCB2_AfterEvolution = CB2_ReturnToField;
        BeginEvolutionScene(&gPlayerParty[0], SPECIES_RAICHU, FALSE, 0);
    }
    if (sTimer == 150) Shot("evolution_start");
    if (sTimer == 330) Shot("evolution_pre");
    if (sTimer > 200 && sTimer < 2400 && (sTimer % 60) == 0) Press(A_BUTTON);
    if (sTimer == 1150) Shot("evolution_mid");
    if (sTimer == 1450) Shot("evolution_done");
    if (sTimer > 1500 && FieldIdle())
    {
        GetBoxMonGenome(&gPlayerParty[0].box, &after);
        Check(GetMonData(&gPlayerParty[0], MON_DATA_SPECIES) == SPECIES_RAICHU && SameGenome(&before, &after), "evolved into Raichu with the same genome");
        NextStage();
    }
    if (sTimer > 60 * 90) { Check(FALSE, "evolution scene did not return to the field"); NextStage(); }
}

static void PC(void)
{
    u32 i;
    sTimer++;
    if (sTimer == 1)
    {
        struct Pokemon m;
        for (i = 0; i < IN_BOX_COUNT; i++) ZeroBoxMonData(&gPokemonStoragePtr->boxes[0][i]);
        for (i = 0; i < 14; i++) { ColourMon(&m, SPECIES_EEVEE, (s8)(-14 + 2 * i), NUM_COAT_LOCI); gPokemonStoragePtr->boxes[0][i] = m.box; }
        ColourMon(&m, SPECIES_PIKACHU, 0, COAT_LOCUS_C); gPokemonStoragePtr->boxes[0][14] = m.box;
        gPokemonStoragePtr->boxes[0][15] = sWildEevee.box;
        gPokemonStoragePtr->boxes[0][16] = sFinal[0].box;
        gPokemonStoragePtr->currentBox = 0;
        LockPlayerFieldControls();
        ShowPokemonStorageSystemPC();
    }
    if (sTimer == 100) Press(A_BUTTON);      // MOVE POKeMON (first in expansion's PC menu)
    if (sTimer == 260) { LogStr("  PC: free OBJ palettes after icons "); LogNum(CountFreeSpritePalettes()); LogStr("\n"); Shot("pc_box_genetic_icons"); }
    if (sTimer == 270) Press(A_BUTTON);      // pick up the first Eevee (held mon keeps its colours)
    if (sTimer == 330) Shot("pc_box_holding");
    if (sTimer == 340) { sStage = ST_DONE; sTimer = 0; }   // the Eevee stages run only in MODE 1 / 2
}

// ---------------------------------------------------------------- Eevee lines for the script (real Day Care code)
// Strategy as in the script: catch a wild female and male Eevee; each generation the Day Care lays 10 eggs
// (TriggerPendingDaycareEgg + GiveEggFromDaycare); the viable ones hatch through the real data path (ScriptHatchMon);
// keep the tallest son and tallest daughter by the HEIGHT the summary shows (cm, ties at random); a sex with no
// hatchling keeps its old parent. "Score vs wild-type" = the pair's tier against an all-ancestral wild Eevee:
// min over the two kept parents of d (completed opposite pairs), so 50 / 20 / 0 = d >= 1 / 2 / 3 for both.
#define EV_EGGS     10
#define EV_MAX_GENS 120
#define EV_LINES    200
struct EvLine
{
    struct Pokemon mum, dad;
    u16 gen, eggs, hatched, failed, gen50, gen20, gen0, firstFailGen;
    u16 hatched50, hatched20, hatched0, eggs0, failed0;
    bool8 stuck;
};
struct EvMilestone { struct Pokemon mons[4]; u16 gen, eggs, hatched, failed; u8 n, scoreF, scoreM, inLine; };
enum { MS_GEN1, MS_GET_ALONG, MS_DONT_LIKE, MS_FIRST_FAIL, MS_FINAL, MS_COUNT };
static EWRAM_DATA struct EvLine sEv = {0};
static EWRAM_DATA struct EvMilestone sMs[MS_COUNT] = {0};
static EWRAM_DATA bool8 sMsTaken[MS_COUNT] = {0};
static EWRAM_DATA struct Pokemon sEvEgg = {0}, sEvWildF = {0}, sEvWildM = {0};
static EWRAM_DATA u8 sEvGen0[EV_LINES] = {0}, sEvGen50[EV_LINES] = {0}, sEvGen20[EV_LINES] = {0};
static EWRAM_DATA u16 sEvHatched0[EV_LINES] = {0};
static EWRAM_DATA u32 sEvSumEggs = 0, sEvSumFailed = 0, sEvStuck = 0, sEvAttempts = 0;

static u32 MonHeightCm(struct Pokemon *mon)
{
    struct Genome g;
    GetBoxMonGenome(&mon->box, &g);
    return GenomeHeightCm(&g, GetMonData(mon, MON_DATA_PERSONALITY), gSpeciesInfo[GetMonData(mon, MON_DATA_SPECIES)].height);
}
static void EvInit(struct EvLine *L)
{
    memset(L, 0, sizeof(*L));
    MakeMon(&L->mum, SPECIES_EEVEE, 20, TRUE, NULL, FALSE);
    MakeMon(&L->dad, SPECIES_EEVEE, 20, FALSE, NULL, FALSE);
}
static u8 PairScoreVsWild(struct EvLine *L)
{
    u32 dF = WildTypeDistance(&L->mum), dM = WildTypeDistance(&L->dad), d = dF < dM ? dF : dM;
    return d >= 3 ? 0 : d == 2 ? 20 : d == 1 ? 50 : 70;
}
static void TakeMilestone(u32 which, struct EvLine *L, struct Pokemon *extra, u32 nExtra)
{
    struct EvMilestone *m = &sMs[which];
    u32 i;
    if (sMsTaken[which]) return;
    sMsTaken[which] = TRUE;
    m->mons[0] = L->mum; m->mons[1] = L->dad; m->n = 2;
    for (i = 0; i < nExtra && m->n < 4; i++) m->mons[m->n++] = extra[i];
    m->gen = L->gen; m->eggs = L->eggs; m->hatched = L->hatched; m->failed = L->failed;
    m->scoreF = WildTypeDistance(&L->mum) >= 3 ? 0 : WildTypeDistance(&L->mum) == 2 ? 20 : WildTypeDistance(&L->mum) == 1 ? 50 : 70;
    m->scoreM = WildTypeDistance(&L->dad) >= 3 ? 0 : WildTypeDistance(&L->dad) == 2 ? 20 : WildTypeDistance(&L->dad) == 1 ? 50 : 70;
    {   // from the genomes, not DaycareScore: that would empty the Day Care in the middle of a generation
        struct Genome gf, gm;
        GetBoxMonGenome(&L->mum.box, &gf); GetBoxMonGenome(&L->dad.box, &gm);
        m->inLine = GenomeCompatibilityScore(&gf, &gm);
    }
}
// one generation; capture = record milestones for the on-camera line
static void EvGeneration(struct EvLine *L, bool8 capture)
{
    struct Pokemon bestF, bestM, *egg;
    u32 e, key, keyF = 0, keyM = 0;
    bool8 haveF = FALSE, haveM = FALSE;
    struct Genome g;
    u8 score;

    SetDaycare(&L->mum, &L->dad);
    for (e = 0; e < EV_EGGS; e++)
    {
        u8 before = gPlayerPartyCount;
        TriggerPendingDaycareEgg();
        GiveEggFromDaycare();
        if (gPlayerPartyCount == before) { L->stuck = TRUE; break; }   // the pair scores 0 with each other
        egg = &gPlayerParty[gPlayerPartyCount - 1];
        GetBoxMonGenome(&egg->box, &g);
        L->eggs++;
        if (g.dead)
        {
            L->failed++;
            if (capture && !sMsTaken[MS_FIRST_FAIL])
            {
                u8 cycles = 1;   // ready to hatch on the next egg cycle, so the capture can walk into "The EGG didn't hatch"
                sEvEgg = *egg;
                SetMonData(&sEvEgg, MON_DATA_FRIENDSHIP, &cycles);
                L->gen++;  // the egg belongs to the generation being bred
                TakeMilestone(MS_FIRST_FAIL, L, &sEvEgg, 1);
                L->gen--;
                sMs[MS_FIRST_FAIL].eggs = L->eggs; sMs[MS_FIRST_FAIL].failed = L->failed; sMs[MS_FIRST_FAIL].hatched = L->hatched;
            }
            ZeroMonData(egg); CalculatePlayerPartyCount();
            continue;
        }
        gSpecialVar_0x8004 = gPlayerPartyCount - 1;
        ScriptHatchMon();                          // the game's hatch data path (no scene)
        L->hatched++;
        key = MonHeightCm(egg) * 8 + (Random() & 7);
        if (GetMonGender(egg) == MON_FEMALE) { if (!haveF || key > keyF) { bestF = *egg; keyF = key; haveF = TRUE; } }
        else                                  { if (!haveM || key > keyM) { bestM = *egg; keyM = key; haveM = TRUE; } }
        ZeroMonData(egg); CalculatePlayerPartyCount();
    }
    ClearDaycare();
    if (haveF) L->mum = bestF;
    if (haveM) L->dad = bestM;
    L->gen++;
    score = PairScoreVsWild(L);
    if (!L->gen50 && score <= 50) { L->gen50 = L->gen; L->hatched50 = L->hatched; if (capture) TakeMilestone(MS_GET_ALONG, L, NULL, 0); }
    if (!L->gen20 && score <= 20) { L->gen20 = L->gen; L->hatched20 = L->hatched; if (capture) TakeMilestone(MS_DONT_LIKE, L, NULL, 0); }
    if (!L->gen0 && score == 0) { L->gen0 = L->gen; L->hatched0 = L->hatched; L->eggs0 = L->eggs; L->failed0 = L->failed; }
    if (capture && L->gen == 1) TakeMilestone(MS_GEN1, L, NULL, 0);
}

static void SortU8(u8 *a, u32 n) { u32 i, j; u8 t; for (i = 1; i < n; i++) for (j = i; j > 0 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; } }
static void SortU16(u16 *a, u32 n) { u32 i, j; u16 t; for (i = 1; i < n; i++) for (j = i; j > 0 && a[j - 1] > a[j]; j--) { t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; } }

static void EeveeStats(void)
{
    if (sTimer++ == 0)
    {
        LogStr("[eevee stats] 200 Eevee lines through the real Day Care: 10 eggs/gen, keep the tallest son + daughter\n");
        ResetParty();
        MakeMon(&gPlayerParty[0], SPECIES_MUDKIP, 10, FALSE, NULL, FALSE);
        CalculatePlayerPartyCount();
    }
    if (sEv.gen == 0)
        EvInit(&sEv);
    EvGeneration(&sEv, FALSE);
    if (sEv.gen0 || sEv.stuck || sEv.gen >= EV_MAX_GENS)
    {
        sEvGen0[sCount] = sEv.gen0 ? sEv.gen0 : 255;
        sEvGen50[sCount] = sEv.gen50 ? sEv.gen50 : 255;
        sEvGen20[sCount] = sEv.gen20 ? sEv.gen20 : 255;
        sEvHatched0[sCount] = sEv.gen0 ? sEv.hatched0 : 0xFFFF;
        if (sEv.gen0) { sEvSumEggs += sEv.eggs0; sEvSumFailed += sEv.failed0; }
        if (sEv.stuck) sEvStuck++;
        LogStr("  line "); LogNum(sCount + 1); LogStr(": gen0 "); LogNum(sEv.gen0); LogStr(" gens "); LogNum(sEv.gen); LogStr(sEv.stuck ? " stuck\n" : "\n");
        memset(&sEv, 0, sizeof(sEv));
        if (++sCount >= EV_LINES)
        {
            u32 i, reached = 0, sumHatched = 0;
            for (i = 0; i < EV_LINES; i++) if (sEvGen0[i] != 255) { reached++; sumHatched += sEvHatched0[i]; }
            SortU8(sEvGen0, EV_LINES); SortU8(sEvGen50, EV_LINES); SortU8(sEvGen20, EV_LINES); SortU16(sEvHatched0, EV_LINES);
            LogStr("  reached score 0 vs wild-type: "); LogNum(reached); LogStr("/200 within "); LogNum(EV_MAX_GENS); LogStr(" gens; stuck (pair scores 0 with itself): "); LogNum(sEvStuck); LogStr("\n");
            LogStr("  generations to score 0, p10/median/p90: "); LogNum(sEvGen0[EV_LINES / 10]); LogStr(" / "); LogNum(sEvGen0[EV_LINES / 2]); LogStr(" / "); LogNum(sEvGen0[EV_LINES * 9 / 10]); LogStr("\n");
            LogStr("  eggs hatched to score 0: mean x10 "); LogNum(reached ? sumHatched * 10 / reached : 0); LogStr(", median "); LogNum(sEvHatched0[EV_LINES / 2]);
            LogStr(", p10 "); LogNum(sEvHatched0[EV_LINES / 10]); LogStr(", p90 "); LogNum(sEvHatched0[EV_LINES * 9 / 10]); LogStr("\n");
            LogStr("  first gen at 50 (get along), median "); LogNum(sEvGen50[EV_LINES / 2]); LogStr(" (p10 "); LogNum(sEvGen50[EV_LINES / 10]); LogStr(", p90 "); LogNum(sEvGen50[EV_LINES * 9 / 10]); LogStr(")\n");
            LogStr("  first gen at 20 (don't like each other much), median "); LogNum(sEvGen20[EV_LINES / 2]); LogStr(" (p10 "); LogNum(sEvGen20[EV_LINES / 10]); LogStr(", p90 "); LogNum(sEvGen20[EV_LINES * 9 / 10]); LogStr(")\n");
            LogStr("  failed eggs up to score 0: "); LogNum(sEvSumFailed); LogStr(" of "); LogNum(sEvSumEggs); LogStr(" (x1000: "); LogNum(sEvSumEggs ? sEvSumFailed * 1000 / sEvSumEggs : 0); LogStr(")\n");
            Check(reached >= 180, "Eevee lines reach a new species through the real Day Care");
            NextStage();
        }
    }
}

// The on-camera line: the first run that completes, start to finish, with milestone snapshots.
static void EeveeCapture(void)
{
    if (sEv.gen == 0 && !sEv.stuck)
    {
        if (sEvAttempts == 0)
        {
            LogStr("[eevee capture] on-camera line: first run that completes\n");
            ResetParty();
            MakeMon(&gPlayerParty[0], SPECIES_MUDKIP, 10, FALSE, NULL, FALSE);
            CalculatePlayerPartyCount();
        }
        sEvAttempts++;
        memset(sMsTaken, 0, sizeof(sMsTaken));
        EvInit(&sEv);
        LogMon("founder F", &sEv.mum); LogMon("founder M", &sEv.dad);
    }
    EvGeneration(&sEv, TRUE);
    if (sEv.stuck || sEv.gen >= EV_MAX_GENS)
    {
        LogStr("  attempt "); LogNum(sEvAttempts); LogStr(sEv.stuck ? " stuck (pair scores 0 with itself)" : " did not finish"); LogStr(", next\n");
        memset(&sEv, 0, sizeof(sEv));
        return;
    }
    if (sEv.gen0)
    {
        u32 t, steps, all[30], i;
        do MakeMon(&sEvWildF, SPECIES_EEVEE, 20, TRUE, NULL, FALSE); while (MonDerived(&sEvWildF) != 0);
        do MakeMon(&sEvWildM, SPECIES_EEVEE, 20, FALSE, NULL, FALSE); while (MonDerived(&sEvWildM) != 0);
        {
            struct Pokemon extra[2] = {sEvWildF, sEvWildM};
            TakeMilestone(MS_FINAL, &sEv, extra, 2);
        }
        // the final brother x sister: how fast does the real step counter give an egg?
        for (t = 0; t < 30; t++)
        {
            SetDaycare(&sEv.mum, &sEv.dad);
            for (steps = 1; steps < 100000; steps++)
            {
                IncrementDaycareSteps();
                if (FlagGet(FLAG_PENDING_DAYCARE_EGG)) break;
            }
            all[t] = steps;
        }
        ClearDaycare();
        for (i = 1; i < 30; i++) { u32 j, x; for (j = i; j > 0 && all[j - 1] > all[j]; j--) { x = all[j]; all[j] = all[j - 1]; all[j - 1] = x; } }
        LogMon("final F", &sEv.mum); LogMon("final M", &sEv.dad);
        LogStr("  final brother x sister Day Care score "); LogNum(DaycareScore(&sEv.mum, &sEv.dad));
        LogStr(", steps to an egg (real step counter, 30 trials) median "); LogNum(all[15]); LogStr(", p90 "); LogNum(all[27]); LogStr("\n");
        LogStr("CAPTURE_JSON {\"attempt\":"); LogNum(sEvAttempts);
        LogStr(",\"species\":\"EEVEE\",\"eggs_per_generation\":10,\"final_generation\":"); LogNum(sEv.gen0);
        LogStr(",\"eggs_laid\":"); LogNum(sEv.eggs0); LogStr(",\"eggs_hatched\":"); LogNum(sEv.hatched0); LogStr(",\"eggs_failed\":"); LogNum(sEv.failed0);
        LogStr(",\"first_get_along_generation\":"); LogNum(sEv.gen50); LogStr(",\"first_dont_like_generation\":"); LogNum(sEv.gen20);
        LogStr(",\"first_failed_egg_generation\":"); LogNum(sMs[MS_FIRST_FAIL].gen);
        LogStr(",\"final_pair_inline_score\":"); LogNum(DaycareScore(&sEv.mum, &sEv.dad));
        LogStr(",\"final_pair_steps_to_egg_median\":"); LogNum(all[15]); LogStr(",\"final_heights_cm\":["); LogNum(MonHeightCm(&sEv.mum)); LogStr(","); LogNum(MonHeightCm(&sEv.dad));
        LogStr("],\"milestones\":[");
        for (i = 0; i < MS_COUNT; i++)
        {
            static const char *const names[] = {"gen1", "first_get_along", "first_dont_like", "first_failed_egg", "final_species"};
            LogStr(i ? ",{" : "{"); LogStr("\"name\":\""); LogStr(names[i]); LogStr("\",\"generation\":"); LogNum(sMs[i].gen);
            LogStr(",\"eggs_laid\":"); LogNum(sMs[i].eggs); LogStr(",\"eggs_hatched\":"); LogNum(sMs[i].hatched); LogStr(",\"eggs_failed\":"); LogNum(sMs[i].failed);
            LogStr(",\"score_vs_wild_F\":"); LogNum(sMs[i].scoreF); LogStr(",\"score_vs_wild_M\":"); LogNum(sMs[i].scoreM); LogStr(",\"inline_score\":"); LogNum(sMs[i].inLine);
            LogStr(",\"heights_cm\":["); LogNum(MonHeightCm(&sMs[i].mons[0])); LogStr(","); LogNum(MonHeightCm(&sMs[i].mons[1])); LogStr("]}");
        }
        LogStr("]}\n");
        Check(sEv.gen0 > 0, "on-camera Eevee line became a new species");
        NextStage();
    }
}

// ---------------------------------------------------------------- cross-breed hybrid sprites (GAME_SPEC feature 3)
// Each hybrid is a real party Pokemon drawn through the game's own loaders (LoadSpecialPokePic + the species/personality
// palette getter: what battle, summary, PC, trade and evolution use). Every pic + palette goes out through gHybridDump;
// test/hybrid/run.sh decodes them and diffs them against the owner's reference PNGs. Then the hybrids are shown on real
// screens (battle, summary) for screenshots.
struct HybridDump { u32 magic; u16 seq; u16 pad; u8 name[32]; u16 pal[16]; u8 front[64 * 128 / 2]; u8 back[64 * 64 / 2]; };
EWRAM_DATA struct HybridDump gHybridDump = {0};

struct HybridCase { const char *name; u16 body; u16 colour; u16 part0, part1; };
static const struct HybridCase sHybridCases[] =
{   // = tools/hybrid/hybrid_table.py CASES, plus the plain species (no look: must be drawn untouched)
    {"hybrid_pika_x_char", SPECIES_PIKACHU, SPECIES_CHARMANDER, SPECIES_CHARMANDER, SPECIES_NONE},
    {"hybrid_char_x_pika", SPECIES_CHARMANDER, SPECIES_PIKACHU, SPECIES_NONE, SPECIES_NONE},
    {"hybrid_char_x_bulb", SPECIES_CHARMANDER, SPECIES_BULBASAUR, SPECIES_BULBASAUR, SPECIES_NONE},
    {"hybrid_three", SPECIES_CHARMANDER, SPECIES_PIKACHU, SPECIES_BULBASAUR, SPECIES_PIKACHU},
    {"charmander", SPECIES_CHARMANDER, SPECIES_NONE, SPECIES_NONE, SPECIES_NONE},
    {"pikachu", SPECIES_PIKACHU, SPECIES_NONE, SPECIES_NONE, SPECIES_NONE},
};

static void HybridDumpMon(const char *name, struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u32 personality = GetMonData(mon, MON_DATA_PERSONALITY), i;
    LoadSpecialPokePic(gHybridDump.front, species, personality, TRUE);
    LoadSpecialPokePic(gHybridDump.back, species, personality, FALSE);
    CpuCopy16(GetMonSpritePalFromSpeciesAndPersonality(species, FALSE, personality), gHybridDump.pal, sizeof(gHybridDump.pal));
    for (i = 0; name[i] && i < 31; i++) gHybridDump.name[i] = name[i];
    gHybridDump.name[i] = 0;
    gHybridDump.magic = 0x44425948;     // "HYBD"
    gHybridDump.seq++;
    LogStr("  dump  "); LogStr(name); LogStr("\n");
}

static void HybridParty(void)
{
    u32 i;
    struct Genome g = {0};          // wild type: no coat / hue / shiny gene on top of the hybrid colours
    ResetParty();
    HybridSetTestLook(0, NULL);
    for (i = 0; i < ARRAY_COUNT(sHybridCases) && i < PARTY_SIZE; i++)
    {
        const struct HybridCase *c = &sHybridCases[i];
        MakeMon(&gPlayerParty[i], c->body, 20, FALSE, &g, FALSE);   // male: Pikachu keeps its default (non-female) art
        if (c->colour != SPECIES_NONE || c->part0 != SPECIES_NONE)
        {
            struct HybridLook look = {c->colour, {c->part0, c->part1}};
            HybridSetTestLook(GetMonData(&gPlayerParty[i], MON_DATA_PERSONALITY), &look);
        }
    }
    CalculatePlayerPartyCount();
}

static void Hybrid(void)
{
    u32 t;
    sTimer++;
    switch (sSub)
    {
    case 0:     // pixel dumps through the real loaders
        if (sTimer == 1)
        {
            LogStr("[hybrid] cross-breed sprites through LoadSpecialPokePic + the palette getter\n");
            HybridParty();
        }
        if (sTimer >= 10 && (sTimer % 10) == 0)
        {
            u32 i = sTimer / 10 - 1;
            if (i < ARRAY_COUNT(sHybridCases))
            {
                struct HybridLook look;
                bool32 isHybrid = GetBoxMonHybridLook(&gPlayerParty[i].box, &look);
                Check(isHybrid == (sHybridCases[i].colour != SPECIES_NONE), sHybridCases[i].name);
                HybridDumpMon(sHybridCases[i].name, &gPlayerParty[i]);
            }
            else
            {
                struct HybridLook look;
                Check(GetBoxMonHybridLook(&gPlayerParty[3].box, &look) && look.partSpecies[1] == SPECIES_PIKACHU,
                      "three-species look has both parts");
                sSub = 1; sTimer = 0;
            }
        }
        break;
    case 1:     // battle: hybrid_three from the back (player), hybrid_pika_x_char from the front (opponent)
        if (sTimer == 1)
        {
            struct Pokemon lead = gPlayerParty[3];
            ZeroEnemyPartyMons();
            gEnemyParty[0] = gPlayerParty[0];   // same personality -> same look (FindOwnedBoxMon checks the enemy party too)
            gPlayerParty[3] = gPlayerParty[0];
            gPlayerParty[0] = lead;
            SetMoves(&gPlayerParty[0], MOVE_GROWL);
            SetMoves(&gEnemyParty[0], MOVE_GROWL);
            BattleSetup_StartWildBattle();
        }
        if (sTimer < 60 * 12 && (sTimer % 90) == 0) Press(B_BUTTON);
        if (sTimer < 60 * 12) break;
        t = sTimer - 60 * 12;
        if (t == 0) Shot("hybrid_battle");
        if (t >= 10)
        {
            u32 k = (t - 10) % 240;
            if (k == 0) Press(DPAD_RIGHT);
            if (k == 10) Press(DPAD_DOWN);
            if (k == 20) Press(A_BUTTON);       // RUN (retry)
            if (k > 30 && (k % 50) == 0) Press(B_BUTTON);
            if (FieldIdle() && t > 100) { sSub = 2; sTimer = 0; }
            if (t > 240 * 20) { Check(FALSE, "could not run from the hybrid battle"); sSub = 2; sTimer = 0; }
        }
        break;
    case 2:     // summary screen of the three-species hybrid (party slot 1 after the swap)
        if (!FieldIdle() && sTimer < 5) { sTimer = 0; break; }
        if (sTimer == 10) Press(START_BUTTON);
        if (sTimer == 40) Press(DPAD_DOWN);
        if (sTimer == 60) Press(A_BUTTON);       // POKeMON
        if (sTimer == 160) Shot("hybrid_party_menu");
        if (sTimer == 170) Press(A_BUTTON);      // first mon
        if (sTimer == 200) Press(A_BUTTON);      // SUMMARY
        if (sTimer == 330) Shot("hybrid_summary_three");
        if (sTimer == 340) Press(DPAD_DOWN);
        if (sTimer == 460) Shot("hybrid_summary_char_x_pika");
        if (sTimer == 470) Press(DPAD_DOWN);
        if (sTimer == 590) Shot("hybrid_summary_char_x_bulb");
        if (sTimer == 600) Press(DPAD_DOWN);
        if (sTimer == 720) Shot("hybrid_summary_pika_x_char");
        if (sTimer == 730) Press(B_BUTTON);
        if (sTimer == 800) Press(B_BUTTON);
        if (sTimer == 860) Press(B_BUTTON);
        if (sTimer > 900 && MashBUntilIdle())
        {
            HybridSetTestLook(0, NULL);
            NextStage();
            if (gEvoTest.pad == 3) sStage = ST_DONE;   // hybrid-sprites-only mode stops here
        }
        break;
    }
}


// ---------------------------------------------------------------- cross-breeding (GAME_SPEC features 1-9)
// Unit checks of every rule through the game's own functions (Day Care eggs through GiveEggFromDaycare), then the real
// screens: a wild "???" battle on Route 117, its catch, a Pikachu x Charmander egg hatching, the Day Care refusing a male
// hybrid, the summary pages (chromosomes, parents, fertile heart, three type badges) and a battle of the two cross
// directions (2x and 0.5x).
static EWRAM_DATA struct Pokemon sXbPikaChar = {0}, sXbCharPika = {0}, sXbCharBulbF = {0}, sXbThree = {0}, sXbWild = {0};
static EWRAM_DATA struct Pokemon sXbMaleHybrid = {0}, sXbEgg = {0};

static void XbHybrid(struct Pokemon *mon, u16 body, u8 level, bool8 female, u16 donor0, u16 donor1, bool8 fertile)
{
    struct Genome g = {0};
    struct Lineage l = {{donor0, donor1}, fertile};
    MakeMon(mon, body, level, female, &g, FALSE);
    SetBoxMonLineage(&mon->box, &l);
    GiveMonInitialMoveset(mon);
    CalculateMonStats(mon);
}

static bool32 LearnsetHas(const struct LevelUpMove *ls, u16 move, u32 *level)
{
    u32 i;
    for (i = 0; ls[i].move != LEVEL_UP_MOVE_END; i++)
        if (ls[i].move == move) { if (level) *level = ls[i].level; return TRUE; }
    return FALSE;
}

static void LogTypes(const char *label, struct Pokemon *mon)
{
    u8 t[3]; u32 n = GetBoxMonHybridTypes(&mon->box, GetMonData(mon, MON_DATA_SPECIES), t), i;
    LogStr("  "); LogStr(label); LogStr(" types");
    for (i = 0; i < n; i++) { LogStr(" "); LogNum(t[i]); }
    LogStr("\n");
}

static void CrossbreedUnits(void)
{
    struct Genome z = {0};
    struct Lineage l, l2;
    struct Pokemon a, b, *egg;
    u32 i, n, k, lvl, females, fertile, dead, hatched, three;
    u8 t[3], t1, t2;

    LogStr("[crossbreed] units\n");
    // ---- storage: 21 lineage bits in retired fields, checksum and everything else intact
    MakeMon(&a, SPECIES_CHARMANDER, 20, TRUE, &z, FALSE);
    {
        u32 exp = GetMonData(&a, MON_DATA_EXP), otid = GetMonData(&a, MON_DATA_OT_ID), pid = GetMonData(&a, MON_DATA_PERSONALITY);
        u16 gen = 300, tracker = 777;
        struct Genome g0, g1;
        GetBoxMonGenome(&a.box, &g0);
        l.donor[0] = SPECIES_PIKACHU; l.donor[1] = SPECIES_BULBASAUR; l.fertile = TRUE;
        SetBoxMonLineage(&a.box, &l);
        GetBoxMonLineage(&a.box, &l2);
        GetBoxMonGenome(&a.box, &g1);
        Check(l2.donor[0] == SPECIES_PIKACHU && l2.donor[1] == SPECIES_BULBASAUR && l2.fertile, "lineage round trip (donors + fertile bit)");
        Check(!GetMonData(&a, MON_DATA_SANITY_IS_BAD_EGG) && GetMonData(&a, MON_DATA_SPECIES) == SPECIES_CHARMANDER
              && GetMonData(&a, MON_DATA_EXP) == exp && GetMonData(&a, MON_DATA_OT_ID) == otid
              && GetMonData(&a, MON_DATA_PERSONALITY) == pid && SameGenome(&g0, &g1), "lineage write keeps checksum, species, EXP, OT, PID, genome");
        SetMonData(&a, MON_DATA_GENERATION, &gen);
        SetMonData(&a, MON_DATA_EVOLUTION_TRACKER, &tracker);
        SetMonData(&a, MON_DATA_DAYS_SINCE_FORM_CHANGE, &tracker);
        SetMonData(&a, MON_DATA_NICKNAME, gText_ExpandedPlaceholder_Empty);
        GetBoxMonLineage(&a.box, &l2);
        Check(GetMonData(&a, MON_DATA_GENERATION) == 255 && GetMonData(&a, MON_DATA_EVOLUTION_TRACKER) == 0
              && GetMonData(&a, MON_DATA_DAYS_SINCE_FORM_CHANGE) == 0
              && l2.donor[0] == SPECIES_PIKACHU && l2.donor[1] == SPECIES_BULBASAUR && l2.fertile,
              "generation caps at 255; tracker / form-day / nickname writes can't touch the lineage");
        CreateMon(&b, SPECIES_CHARMANDER, 5, 0, OTID_STRUCT_PLAYER_ID);
        Check(!IsBoxMonHybrid(&b.box), "a new Pokemon is not a hybrid");
    }

    // ---- feature 1: chromosomes
    {
        static const u16 sp[] = {SPECIES_PIKACHU, SPECIES_PICHU, SPECIES_RAICHU, SPECIES_CHARMANDER, SPECIES_CHARIZARD,
                                 SPECIES_BULBASAUR, SPECIES_EEVEE, SPECIES_VAPOREON, SPECIES_MAGIKARP, SPECIES_GYARADOS,
                                 SPECIES_WAILORD, SPECIES_VOLBEAT, SPECIES_ILLUMISE, SPECIES_ODDISH, SPECIES_SEEDOT, SPECIES_POOCHYENA};
        static const u8 want[] = {40, 40, 42, 52, 52, 48, 38, 38, 104, 104, 44, 0, 0, 0, 0, 0};
        bool32 ok = TRUE, fam = TRUE;
        for (i = 0; i < ARRAY_COUNT(sp); i++)
        {
            LogStr("  chromosomes "); LogNum(sp[i]); LogStr(" = "); LogNum(GetSpeciesChromosomes(sp[i])); LogStr("\n");
            if (want[i] && GetSpeciesChromosomes(sp[i]) != want[i]) ok = FALSE;
        }
        Check(ok, "the video's fixed chromosome numbers (Pikachu/Pichu 40, Raichu 42, Charmander 52, Bulbasaur 48, Eevee 38, Magikarp 104, Wailord 44)");
        for (i = 1; i <= SPECIES_DEOXYS; i++)
            if (IsSpeciesEnabled(i) && GET_BASE_SPECIES_ID(i) != SPECIES_RAICHU && GetSpeciesChromosomes(i) != GetSpeciesChromosomes(CrossbreedFamily(i)))
                { fam = FALSE; LogStr("  family mismatch "); LogNum(i); LogStr("\n"); }
        Check(fam, "every Gen 1-3 species has its family's number (Raichu is the one override)");
        Check(GetSpeciesChromosomes(SPECIES_VOLBEAT) != GetSpeciesChromosomes(SPECIES_POOCHYENA)
              && (GetSpeciesChromosomes(SPECIES_ILLUMISE) - GetSpeciesChromosomes(SPECIES_VOLBEAT) <= 2), "related species have close numbers (Volbeat/Illumise within 2)");
    }

    // ---- feature 2: cross-species eggs; Ditto and the Undiscovered group stay vanilla
    MakeMon(&a, SPECIES_PIKACHU, 20, TRUE, &z, FALSE);
    MakeMon(&b, SPECIES_CHARMANDER, 20, FALSE, &z, FALSE);
    Check(DaycareScore(&a, &b) == PARENTS_LOW_COMPATIBILITY, "Pikachu F x Charmander M (no shared egg group) can make an egg (score 20)");
    MakeMon(&b, SPECIES_BULBASAUR, 20, FALSE, &z, FALSE);
    MakeMon(&a, SPECIES_CHARMANDER, 20, TRUE, &z, FALSE);
    Check(DaycareScore(&a, &b) == PARENTS_LOW_COMPATIBILITY, "Charmander F x Bulbasaur M (shared Monster group) score 20");
    MakeMon(&a, SPECIES_PICHU, 20, TRUE, &z, FALSE);
    Check(DaycareScore(&a, &b) == PARENTS_INCOMPATIBLE, "Undiscovered group (Pichu) still can't breed");
    MakeMon(&a, SPECIES_DITTO, 20, TRUE, &z, FALSE);
    Check(DaycareScore(&a, &b) == PARENTS_LOW_COMPATIBILITY, "Ditto x Bulbasaur (same OT) stays vanilla (20)");
    ResetParty();
    egg = DaycareEggOf(SPECIES_DITTO, SPECIES_BULBASAUR, &z, &z, FALSE);
    Check(GetMonData(egg, MON_DATA_SPECIES) == SPECIES_BULBASAUR && !IsBoxMonHybrid(&egg->box), "Ditto egg = the other parent, not a hybrid");
    ResetParty();
    egg = DaycareEggOf(SPECIES_PIKACHU, SPECIES_RAICHU, &z, &z, FALSE);
    Check(GetMonData(egg, MON_DATA_SPECIES) == SPECIES_PICHU && !IsBoxMonHybrid(&egg->box), "same family (Pikachu x Raichu) = a normal egg");

    // ---- features 2+3: the hybrid egg: mother's body, father's lineage, the sprite look
    ResetParty();
    egg = DaycareEggOf(SPECIES_CHARMANDER, SPECIES_PIKACHU, &z, &z, FALSE);
    GetBoxMonLineage(&egg->box, &l);
    Check(GetMonData(egg, MON_DATA_SPECIES) == SPECIES_CHARMANDER && l.donor[0] == SPECIES_PIKACHU && l.donor[1] == SPECIES_NONE,
          "Charmander F x Pikachu M egg: Charmander body, Pikachu lineage");
    ResetParty();
    egg = DaycareEggOf(SPECIES_PIKACHU, SPECIES_CHARMANDER, &z, &z, FALSE);
    GetBoxMonLineage(&egg->box, &l);
    sXbEgg = *egg;
    Check(GetMonData(egg, MON_DATA_SPECIES) == SPECIES_PICHU && l.donor[0] == SPECIES_CHARMANDER,
          "Pikachu F x Charmander M egg: the mother's line hatches (Pichu), Charmander lineage");
    ResetParty();
    egg = DaycareEggOf(SPECIES_ILLUMISE, SPECIES_VOLBEAT, &z, &z, FALSE);
    GetBoxMonLineage(&egg->box, &l);
    Check(GetMonData(egg, MON_DATA_SPECIES) == SPECIES_ILLUMISE && l.donor[0] == SPECIES_VOLBEAT, "Illumise F x Volbeat M = an Illumise-body hybrid (no coin flip)");
    XbHybrid(&sXbPikaChar, SPECIES_PIKACHU, 20, FALSE, SPECIES_CHARMANDER, SPECIES_NONE, FALSE);
    XbHybrid(&sXbCharPika, SPECIES_CHARMANDER, 20, FALSE, SPECIES_PIKACHU, SPECIES_NONE, FALSE);
    XbHybrid(&sXbCharBulbF, SPECIES_CHARMANDER, 20, TRUE, SPECIES_BULBASAUR, SPECIES_NONE, TRUE);
    XbHybrid(&sXbThree, SPECIES_CHARMANDER, 20, TRUE, SPECIES_PIKACHU, SPECIES_BULBASAUR, TRUE);
    {
        struct HybridLook look;
        Check(GetBoxMonHybridLook(&sXbPikaChar.box, &look) && look.colourSpecies == SPECIES_CHARMANDER
              && look.partSpecies[0] == SPECIES_CHARMANDER && look.partSpecies[1] == SPECIES_NONE, "sprite look: Charmander colours + flame");
        Check(GetBoxMonHybridLook(&sXbThree.box, &look) && look.colourSpecies == SPECIES_PIKACHU
              && look.partSpecies[0] == SPECIES_BULBASAUR && look.partSpecies[1] == SPECIES_PIKACHU, "sprite look: three-species = Pikachu colours, bulb then ears");
        Check(!GetBoxMonHybridLook(&b.box, &look), "no look for a plain Pokemon");
    }

    // ---- feature 4: types
    GetBoxMonBattleTypes(&sXbPikaChar.box, SPECIES_PIKACHU, &t1, &t2);
    Check(t1 == TYPE_ELECTRIC && t2 == TYPE_FIRE, "Pikachu x Charmander is ELECTRIC / FIRE in battle");
    n = GetBoxMonHybridTypes(&sXbThree.box, SPECIES_CHARMANDER, t);
    LogTypes("three-species", &sXbThree);
    Check(n == 3 && t[0] == TYPE_FIRE && t[1] == TYPE_GRASS && t[2] == TYPE_ELECTRIC, "three-species shows FIRE / GRASS / ELECTRIC");
    GetBoxMonBattleTypes(&sXbThree.box, SPECIES_CHARMANDER, &t1, &t2);
    Check(t1 == TYPE_FIRE && t2 == TYPE_ELECTRIC, "three-species battles as FIRE / ELECTRIC (engine limit: 2 types)");

    // ---- feature 5: moves from both sides
    {
        const struct LevelUpMove *ls = CrossbreedLearnset(&sXbPikaChar.box, SPECIES_PIKACHU);
        u32 emberLevel = 0;
        bool32 learned = FALSE;
        Check(LearnsetHas(ls, MOVE_THUNDER_SHOCK, NULL) && LearnsetHas(ls, MOVE_EMBER, &emberLevel), "hybrid learnset has Thunder Shock and Ember");
        Check(!LearnsetHas(GetSpeciesLevelUpLearnset(SPECIES_PIKACHU), MOVE_EMBER, NULL), "(plain Pikachu does not learn Ember)");
        XbHybrid(&a, SPECIES_PIKACHU, emberLevel, FALSE, SPECIES_CHARMANDER, SPECIES_NONE, FALSE);
        SetMoves(&a, MOVE_THUNDER_SHOCK);
        for (k = MonTryLearningNewMoveAtLevel(&a, TRUE, emberLevel); k != MOVE_NONE; k = MonTryLearningNewMoveAtLevel(&a, FALSE, emberLevel))
            if (k == MOVE_EMBER) learned = TRUE;
        LogStr("  Ember at level "); LogNum(emberLevel); LogStr("\n");
        Check(learned, "a Pikachu x Charmander learns Ember on level-up");
    }

    // ---- feature 6: parent-of-origin growth (liger / tigon)
    LogStr("  scale x256: Pikachu x Charmander "); LogNum(BoxMonDisplayScale(&sXbPikaChar.box, 512));
    LogStr(", Charmander x Pikachu "); LogNum(BoxMonDisplayScale(&sXbCharPika.box, 512));
    LogStr("; height cm "); LogNum(BoxMonHeightCm(&sXbPikaChar.box)); LogStr(" / "); LogNum(BoxMonHeightCm(&sXbCharPika.box)); LogStr("\n");
    Check(BoxMonDisplayScale(&sXbPikaChar.box, 512) == 512 && BoxMonHeightCm(&sXbPikaChar.box) > 60,
          "Charmander dad x Pikachu mom: drawn 2x, taller than both parents (Pikachu 0.4 m, Charmander 0.6 m)");
    Check(BoxMonDisplayScale(&sXbCharPika.box, 512) == 128 && BoxMonHeightCm(&sXbCharPika.box) < 40,
          "Pikachu dad x Charmander mom: drawn 0.5x, smaller than both parents");
    MakeMon(&a, SPECIES_PIKACHU, 20, TRUE, &z, FALSE);
    Check(BoxMonDisplayScale(&a.box, 512) == 256, "a plain Pikachu is drawn 1x (growth only acts in hybrids)");

    // ---- feature 7: Haldane's rule and fertility
    XbHybrid(&sXbMaleHybrid, SPECIES_CHARMANDER, 20, FALSE, SPECIES_BULBASAUR, SPECIES_NONE, TRUE);
    Check(IsBoxMonMaleHybrid(&sXbMaleHybrid.box) && !IsBoxMonFertile(&sXbMaleHybrid.box), "male hybrid is sterile even with the fertile bit");
    MakeMon(&a, SPECIES_CHARMANDER, 20, TRUE, &z, FALSE);
    Check(DaycareScore(&a, &sXbMaleHybrid) == PARENTS_INCOMPATIBLE, "male hybrid x female: no egg");
    MakeMon(&b, SPECIES_DITTO, 20, FALSE, &z, FALSE);
    Check(DaycareScore(&b, &sXbMaleHybrid) == PARENTS_INCOMPATIBLE, "male hybrid x Ditto: no egg");
    XbHybrid(&a, SPECIES_PIKACHU, 20, TRUE, SPECIES_CHARMANDER, SPECIES_NONE, FALSE);
    MakeMon(&b, SPECIES_PIKACHU, 20, FALSE, &z, FALSE);
    Check(DaycareScore(&a, &b) == PARENTS_INCOMPATIBLE, "an infertile female hybrid: no egg");
    MakeMon(&b, SPECIES_CHARMANDER, 20, FALSE, &z, FALSE);
    Check(DaycareScore(&sXbCharBulbF, &b) == PARENTS_LOW_COMPATIBILITY, "a fertile female hybrid can breed");
    LogStr("  fertility /256: Pikachu x Charmander "); LogNum(HybridFertilityChance256(40, 52));
    LogStr(", Charmander x Bulbasaur "); LogNum(HybridFertilityChance256(52, 48)); LogStr("\n");
    for (k = 0; k < 2; k++)
    {
        females = fertile = 0;
        for (i = 0; i < 2000 && females < 60; i++)   // 60 daughters (Charmander is 1 in 8 female)
        {
            ResetParty();
            egg = k == 0 ? DaycareEggOf(SPECIES_PIKACHU, SPECIES_CHARMANDER, &z, &z, FALSE)
                         : DaycareEggOf(SPECIES_CHARMANDER, SPECIES_BULBASAUR, &z, &z, FALSE);
            if (GetMonGender(egg) == MON_FEMALE) { females++; fertile += IsBoxMonFertile(&egg->box); }
        }
        LogStr(k == 0 ? "  Pikachu x Charmander daughters fertile " : "  Charmander x Bulbasaur daughters fertile ");
        LogNum(fertile); LogStr(" / "); LogNum(females); LogStr("\n");
        if (k == 0) Check(fertile * 20 <= females, "Pikachu x Charmander daughters: fertility ~0 (the mule)");
        else Check(fertile * 2 >= females && fertile * 10 <= females * 9, "Charmander x Bulbasaur daughters: mostly fertile (~67%)");
    }

    // ---- feature 9: three species
    MakeMon(&b, SPECIES_PIKACHU, 20, FALSE, &z, FALSE);
    Check(DaycareScore(&sXbCharBulbF, &b) == PARENTS_LOW_COMPATIBILITY && IsThirdSpeciesCross(&sXbCharBulbF.box, &b.box),
          "fertile Charmander x Bulbasaur female x Pikachu M: a third-species cross");
    dead = hatched = three = 0;
    for (i = 0; i < 200; i++)
    {
        struct Genome g;
        ResetParty();
        gPlayerParty[0] = sXbCharBulbF;   // keep her lineage: DaycareEggOf would rebuild a plain mother
        SetDaycare(&sXbCharBulbF, &b);
        TriggerPendingDaycareEgg();
        GiveEggFromDaycare();
        egg = &gPlayerParty[gPlayerPartyCount - 1];
        GetBoxMonGenome(&egg->box, &g);
        if (GenomeIsLethal(&g)) { dead++; continue; }
        hatched++;
        GetBoxMonLineage(&egg->box, &l);
        n = GetBoxMonHybridTypes(&egg->box, GetMonData(egg, MON_DATA_SPECIES), t);
        if (GetMonData(egg, MON_DATA_SPECIES) == SPECIES_CHARMANDER && l.donor[0] == SPECIES_PIKACHU && l.donor[1] == SPECIES_BULBASAUR
         && n == 3 && (GetMonGender(egg) != MON_FEMALE || l.fertile))
            three++;
    }
    ClearDaycare();
    LogStr("  third-species eggs: "); LogNum(dead); LogStr(" never hatch, "); LogNum(hatched); LogStr(" hatch\n");
    Check(dead >= 150 && hatched >= 8 && hatched <= 45, "most third-species eggs fail; about 1 in 8 hatches");
    Check(three == hatched, "every hatched one is Charmander body + Bulbasaur + Pikachu, 3 types, daughters fertile");
    MakeMon(&b, SPECIES_CHARMANDER, 20, FALSE, &z, FALSE);
    for (i = 0, dead = 0; i < 40; i++)
    {
        struct Genome g;
        ResetParty();
        SetDaycare(&sXbCharBulbF, &b);
        TriggerPendingDaycareEgg();
        GiveEggFromDaycare();
        GetBoxMonGenome(&gPlayerParty[gPlayerPartyCount - 1].box, &g);
        dead += GenomeIsLethal(&g);
    }
    ClearDaycare();
    LogStr("  backcross eggs that fail (speciation mutations only): "); LogNum(dead); LogStr(" / 40\n");
    Check(dead <= 8, "a backcross (hybrid female x Charmander) hatches normally");

    // ---- feature 8: wild hybrids
    {
        static const u16 r117[] = {SPECIES_POOCHYENA, SPECIES_ODDISH, SPECIES_MARILL, SPECIES_ILLUMISE, SPECIES_VOLBEAT, SPECIES_SEEDOT};
        u32 j;
        LogStr("  Route 117 related pairs (mother x father):");
        for (i = 0; i < ARRAY_COUNT(r117); i++)
            for (j = 0; j < ARRAY_COUNT(r117); j++)
                if (WildHybridPartnerOk(r117[i], r117[j])) { LogStr(" "); LogNum(r117[i]); LogStr("x"); LogNum(r117[j]); }
        LogStr("\n");
    }
    Check(WildHybridPartnerOk(SPECIES_ILLUMISE, SPECIES_VOLBEAT) && !WildHybridPartnerOk(SPECIES_VOLBEAT, SPECIES_ILLUMISE)
          && !WildHybridPartnerOk(SPECIES_MARILL, SPECIES_ODDISH) && !WildHybridPartnerOk(SPECIES_ODDISH, SPECIES_POOCHYENA),
          "Route 117: Illumise x Volbeat are related (Volbeat can't be the mother); Marill/Oddish/Poochyena aren't");
    {
        const struct WildPokemonInfo *info = gWildMonHeaders[GetCurrentMapWildMonHeaderId()].encounterTypes[0].landMonsInfo;
        u32 illumise = 0, hybrids = 0, other = 0, all = 0;
        for (i = 0; i < 1600; i++)
        {
            TryGenerateWildMon(info, WILD_AREA_LAND, 0);
            all += IsBoxMonHybrid(&gEnemyParty[0].box);
            if (GetMonData(&gEnemyParty[0], MON_DATA_SPECIES) == SPECIES_ILLUMISE)
            {
                illumise++;
                hybrids += IsBoxMonHybrid(&gEnemyParty[0].box);
            }
            else if (IsBoxMonHybrid(&gEnemyParty[0].box))
            {
                struct Lineage wl;
                GetBoxMonLineage(&gEnemyParty[0].box, &wl);
                if (!WildHybridPartnerOk(GetMonData(&gEnemyParty[0], MON_DATA_SPECIES), wl.donor[0]))
                    other++;   // a hybrid of two species that aren't a related pair
            }
        }
        LogStr("  Route 117 x1600: Illumise "); LogNum(illumise); LogStr(", of them hybrids "); LogNum(hybrids);
        LogStr("; hybrids of any species "); LogNum(all); LogStr("\n");
        Check(hybrids * 32 >= illumise && hybrids * 6 <= illumise && other == 0, "wild Illumise are Volbeat hybrids about 1 in 16; only related pairs");
    }
}

static void Crossbreed(void)
{
    u32 t;
    sTimer++;
    switch (sSub)
    {
    case 0:
        if (!FieldIdle()) { sTimer = 0; break; }
        if (sTimer < 10) break;
        CrossbreedUnits();
        ResetParty();
        sSub = 1; sTimer = 0;
        break;
    case 1:     // a wild hybrid on Route 117 through the real encounter path: "Wild ??? appeared!"
        if (sTimer == 1)
        {
            const struct WildPokemonInfo *info = gWildMonHeaders[GetCurrentMapWildMonHeaderId()].encounterTypes[0].landMonsInfo;
            u32 i;
            gCrossbreedForceWildHybrid = TRUE;
            for (i = 0; i < 200; i++)
            {
                TryGenerateWildMon(info, WILD_AREA_LAND, 0);
                if (GetMonData(&gEnemyParty[0], MON_DATA_SPECIES) == SPECIES_ILLUMISE && IsBoxMonHybrid(&gEnemyParty[0].box))
                    break;
            }
            gCrossbreedForceWildHybrid = FALSE;
            {
                u8 name[POKEMON_NAME_LENGTH + 1];
                GetMonData(&gEnemyParty[0], MON_DATA_NICKNAME, name);
                Check(IsWildHybridName(name) && IsBoxMonHybrid(&gEnemyParty[0].box), "wild Illumise x Volbeat hybrid is named ??? in battle");
            }
            gPlayerParty[0] = sXbPikaChar;
            CalculatePlayerPartyCount();
            sXbWild = gEnemyParty[0];
            BattleSetup_StartWildBattle();
        }
        if (sTimer == 60 * 5) Shot("xb_wild_hybrid_appeared");
        if (sTimer == 60 * 6) Shot("xb_wild_hybrid_appeared_b");
        if (sTimer < 60 * 12 && sTimer > 60 * 7 && (sTimer % 90) == 0) Press(B_BUTTON);
        if (sTimer < 60 * 12) break;
        t = sTimer - 60 * 12;
        if (t == 0) Shot("xb_wild_hybrid_battle");
        {
            u32 k = t % 240;
            if (k == 10) Press(DPAD_RIGHT);
            if (k == 20) Press(DPAD_DOWN);
            if (k == 30) Press(A_BUTTON);       // RUN (retry)
            if (k > 40 && (k % 50) == 0) Press(B_BUTTON);
            if (FieldIdle() && t > 100) { sSub = 2; sTimer = 0; }
            if (t > 240 * 20) { Check(FALSE, "could not run from the wild hybrid"); sSub = 2; sTimer = 0; }
        }
        break;
    case 2:     // caught: identified, then a real egg hatch (Pikachu F x Charmander M)
        if (sTimer == 1 && !FieldIdle()) { sTimer = 0; break; }
        if (sTimer == 1)
        {
            u8 name[POKEMON_NAME_LENGTH + 1];
            ResetParty();
            GiveCapturedMonToPlayer(&sXbWild);
            GetMonData(&gPlayerParty[0], MON_DATA_NICKNAME, name);
            Check(!IsWildHybridName(name) && IsBoxMonHybrid(&gPlayerParty[0].box), "caught: the ??? gets its species name, stays a hybrid");
            sXbWild = gPlayerParty[0];
            ResetParty();
            gPlayerParty[0] = sXbCharPika;
            gPlayerParty[1] = sXbEgg;
            CalculatePlayerPartyCount();
            ArmHatch();
            TakeStep();
            sLastHatchState = 0xFF;
            sCount = 0;
        }
        else
        {
            u8 state = EvoTest_EggHatchState();
            if (state != sLastHatchState) { LogStr("  hatch state "); LogNum(state); LogStr(" at "); LogNum(sTimer); LogStr("\n"); }
            if (state != 0xFF) sCount = 1;   // the hatch scene has started
            if (sCount == 0)
            {
                if (!FieldIdle() && (sTimer % 45) == 0) Press(B_BUTTON);   // the hatch script's "Oh?"
                if (FieldIdle() && (sTimer % 60) == 0)
                {
                    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
                    LogStr("  hatch try: steps "); LogNum(gSaveBlock1Ptr->daycare.stepCounter); LogStr(" egg friendship ");
                    LogNum(GetMonData(&gPlayerParty[1], MON_DATA_FRIENDSHIP)); LogStr(" isEgg "); LogNum(GetMonData(&gPlayerParty[1], MON_DATA_IS_EGG));
                    LogStr(" at "); LogNum(pl->currentCoords.x); LogStr(","); LogNum(pl->currentCoords.y); LogStr("\n");
                    ArmHatch(); TakeStep();
                }
                if (sTimer > 60 * 30) { Check(FALSE, "hybrid egg never started hatching"); sSub = 3; sTimer = 0; }
                break;
            }
            if (state == 6 && sLastHatchState != 6) sStateTicks = 0;
            sLastHatchState = state;
            if (state == 6 && ++sStateTicks == 50) Shot("xb_hatch_pikachu_x_charmander");
            if (sTimer > 30 && (state == 0xFF || state >= 9))
            {
                if (MashBUntilIdle())
                {
                    struct Lineage l;
                    GetBoxMonLineage(&gPlayerParty[1].box, &l);
                    Check(!GetMonData(&gPlayerParty[1], MON_DATA_IS_EGG) && l.donor[0] == SPECIES_CHARMANDER, "the hybrid egg hatched, lineage intact");
                    sSub = 3; sTimer = 0;
                }
            }
            else if (sTimer > 60 * 60) { Check(FALSE, "hybrid hatch did not finish"); sSub = 3; sTimer = 0; }
        }
        break;
    case 3:     // the Day Care won't take a male hybrid: the real deposit check + the refusal text
        if (sTimer == 1)
        {
            if (!FieldIdle()) { sTimer = 0; break; }
            ResetParty();
            gPlayerParty[0] = sXbCharBulbF;
            gPlayerParty[1] = sXbMaleHybrid;
            CalculatePlayerPartyCount();
            gSpecialVar_0x8004 = 1;
            CrossbreedIsSelectedMonMaleHybrid();
            Check(gSpecialVar_Result == TRUE, "deposit check: the male hybrid is refused");
            gSpecialVar_0x8004 = 0;
            CrossbreedIsSelectedMonMaleHybrid();
            Check(gSpecialVar_Result == FALSE, "deposit check: a female hybrid is taken");
            gSpecialVar_0x8004 = 1;
            GetSelectedMonNicknameAndSpecies();
            ScriptContext_SetupScript(Route117_PokemonDayCare_EventScript_RefuseMaleHybrid);
        }
        if (sTimer == 120) Shot("xb_daycare_refuses_male_hybrid");
        if (sTimer == 130) Press(A_BUTTON);
        if (sTimer == 220) Shot("xb_daycare_refuses_male_hybrid_2");
        if (sTimer > 230 && (sTimer % 40) == 0) Press(A_BUTTON);
        if (sTimer > 300 && FieldIdle()) { sSub = 4; sTimer = 0; }
        if (sTimer > 1500) { Check(FALSE, "refusal script did not finish"); sSub = 4; sTimer = 0; }
        break;
    case 4:     // summary INFO pages: chromosomes, parents, fertile heart, three type badges
        if (sTimer == 1)
        {
            struct Genome z = {0}; struct Pokemon plain;
            ResetParty();
            gPlayerParty[0] = sXbPikaChar;
            gPlayerParty[1] = sXbThree;
            gPlayerParty[2] = sXbCharBulbF;
            gPlayerParty[3] = sXbWild;
            gPlayerParty[4] = sXbCharPika;
            MakeMon(&plain, SPECIES_PIKACHU, 20, FALSE, &z, FALSE);
            gPlayerParty[5] = plain;
            CalculatePlayerPartyCount();
        }
        if (sTimer == 10) Press(START_BUTTON);
        if (sTimer == 40) Press(DPAD_DOWN);
        if (sTimer == 60) Press(A_BUTTON);       // POKeMON
        if (sTimer == 160) Shot("xb_party_menu");
        if (sTimer == 170) Press(A_BUTTON);      // first mon
        if (sTimer == 200) Press(A_BUTTON);      // SUMMARY
        {
            static const char *const names[] = {"xb_summary_pika_x_char", "xb_summary_three_species", "xb_summary_char_x_bulb_fertile",
                                                "xb_summary_wild_illumise_x_volbeat", "xb_summary_char_x_pika", "xb_summary_plain_pikachu"};
            u32 i;
            for (i = 0; i < 6; i++)
            {
                if (sTimer == 330 + i * 130) Shot(names[i]);
                if (sTimer == 340 + i * 130 && i < 5) Press(DPAD_DOWN);
            }
        }
        if (sTimer == 1100) Press(B_BUTTON);
        if (sTimer == 1170) Press(B_BUTTON);
        if (sTimer == 1230) Press(B_BUTTON);
        if (sTimer > 1270 && MashBUntilIdle()) { sSub = 5; sTimer = 0; }
        break;
    case 5:     // battle: Pikachu x Charmander (2x, ELECTRIC/FIRE, uses Ember) vs Charmander x Pikachu (0.5x)
        if (sTimer == 1)
        {
            ResetParty();
            gPlayerParty[0] = sXbPikaChar;
            CalculatePlayerPartyCount();
            SetMoves(&gPlayerParty[0], MOVE_EMBER);
            SetMonMoveSlot(&gPlayerParty[0], MOVE_THUNDER_SHOCK, 1);
            ZeroEnemyPartyMons();
            gEnemyParty[0] = sXbCharPika;
            SetMoves(&gEnemyParty[0], MOVE_GROWL);
            BattleSetup_StartWildBattle();
        }
        if (sTimer < 60 * 12 && (sTimer % 90) == 0) Press(B_BUTTON);
        if (sTimer < 60 * 12) break;
        t = sTimer - 60 * 12;
        if (t == 0)
        {
            u32 p = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT), o = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
            LogStr("  battle types: player "); LogNum(gBattleMons[p].types[0]); LogStr("/"); LogNum(gBattleMons[p].types[1]);
            LogStr(", enemy "); LogNum(gBattleMons[o].types[0]); LogStr("/"); LogNum(gBattleMons[o].types[1]);
            LogStr("; scale x256 player "); LogNum(GetSpriteDisplayScale(gBattlerSpriteIds[p]));
            LogStr(", enemy "); LogNum(GetSpriteDisplayScale(gBattlerSpriteIds[o])); LogStr("\n");
            Check(gBattleMons[p].types[0] == TYPE_ELECTRIC && gBattleMons[p].types[1] == TYPE_FIRE
                  && gBattleMons[o].types[0] == TYPE_FIRE && gBattleMons[o].types[1] == TYPE_ELECTRIC, "battle typing: one type from each parent");
            Check(GetSpriteDisplayScale(gBattlerSpriteIds[p]) == 512 && GetSpriteDisplayScale(gBattlerSpriteIds[o]) == 128,
                  "battle sprites: 2x and 0.5x");
            Shot("xb_battle_liger_vs_tigon");
        }
        if (t == 10) Press(A_BUTTON);           // FIGHT
        if (t == 40) Press(A_BUTTON);           // Ember
        if (t == 150) Shot("xb_battle_ember");
        if (t == 210) Shot("xb_battle_ember_b");
        if (t > 300 && t < 800 && (t % 60) == 0) Press(B_BUTTON);
        if (t >= 810)
        {
            u32 k = (t - 810) % 240;
            if (k == 0) Press(DPAD_RIGHT);
            if (k == 10) Press(DPAD_DOWN);
            if (k == 20) Press(A_BUTTON);
            if (k > 30 && (k % 50) == 0) Press(B_BUTTON);
            if (FieldIdle() && t > 900) NextStage();
            if (t > 810 + 240 * 20) { Check(FALSE, "could not run from the hybrid battle"); NextStage(); }
        }
        break;
    }
}

static void CaptureSaves(void)
{
    static const char *const names[] = {"eevee_01_gen1.sav", "eevee_02_first_get_along.sav", "eevee_03_first_dont_like.sav", "eevee_04_first_failed_egg.sav", "eevee_05_final_species.sav"};
    u32 i;
    if (sTimer++ < 20) return;
    sTimer = 0;
    if (sSub >= MS_COUNT) { NextStage(); return; }
    ResetParty(); ClearDaycare();
    for (i = 0; i < sMs[sSub].n; i++) gPlayerParty[i] = sMs[sSub].mons[i];
    CalculatePlayerPartyCount();
    TrySavingData(SAVE_NORMAL);
    DumpSave(names[sSub]);
    sSub++;
}

void EvoTest_Frame(void)
{
#if SL_TEST
    SlTest_Frame(); return;
#endif
    if (gEvoTest.magic != 0x31545645) { gEvoTest.magic = 0x31545645; LogStr("Emerald Evolved ROM test\n"); }
    if (gEvoTest.finished) return;
    ApplyHold();
    if (sHoldFrames != 0) return;
    switch (sStage)
    {
    case ST_BOOT: Boot(); break;
    case ST_SETUP: if (FieldIdle()) Setup(); break;
    case ST_UNIT: Unit(); break;
    case ST_REPLICATES: Replicates(); break;
    case ST_LINE: Line(); break;
    case ST_DAYCARE: DaycareMan(); break;
    case ST_EGGS: if (FieldIdle()) Eggs(); break;
    case ST_HATCH: if (sSub != 0 || FieldIdle()) Hatch(); break;
    case ST_DEAD_EGG: if (sSub != 0 || FieldIdle()) DeadEggHatch(); break;
    case ST_SAVES: if (FieldIdle()) Saves(); break;
    case ST_FOLLOWER: if (sTimer != 0 || FieldIdle()) Follower(); break;
    case ST_GRASS: if (sTimer != 0 || FieldIdle()) Grass(); break;
    case ST_BATTLE: if (sSub != 0 || FieldIdle()) Battle(); break;
    case ST_PARTY: if (sTimer != 0 || FieldIdle()) Party(); break;
    case ST_EVOLVE: if (sTimer != 0 || FieldIdle()) Evolve(); break;
    case ST_PC: if (sTimer != 0 || FieldIdle()) PC(); break;
    case ST_EEVEE_STATS: if (sTimer != 0 || FieldIdle()) EeveeStats(); break;
    case ST_EEVEE_CAPTURE: EeveeCapture(); break;
    case ST_CAPTURE_SAVES: if (FieldIdle()) CaptureSaves(); break;
    case ST_HYBRID: if (sSub != 0 || sTimer != 0 || FieldIdle()) Hybrid(); break;
    case ST_CROSSBREED: Crossbreed(); break;
    case ST_DONE:
        LogNum(gEvoTest.checks); LogStr(" checks, "); LogNum(gEvoTest.fails); LogStr(" failed\n");
        gEvoTest.finished = gEvoTest.fails ? 2 : 1;
        break;
    }
}

void EvoTest_NewGame(void)
{
#if SL_TEST
    SlTest_NewGame(); return;
#endif
    static const u8 name[] = _("EVO");
    int i;
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++) gSaveBlock2Ptr->playerName[i] = (i < 3) ? name[i] : EOS;
    gSaveBlock2Ptr->playerGender = MALE;
    SetWarpDestination(MAP_GROUP(MAP_ROUTE117), MAP_NUM(MAP_ROUTE117), WARP_ID_NONE, 47, 5);
    WarpIntoMap();
}
#endif
