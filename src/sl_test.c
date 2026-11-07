// Softlock hunt driver (SL_TEST=1 builds, which also set EVO_TEST=1 for the boot shortcut; writes
// pokeemerald_sltest.gba). The headless runner (test/softlock/sl.lua) writes a scenario number into gSlTest before the
// new game starts; this file sets the scenario up and reports the field state every frame. The runner presses the
// buttons and decides whether the player got their controls back.
#include "global.h"
#if SL_TEST
#include "main.h"
#include "pokemon.h"
#include "overworld.h"
#include "event_data.h"
#include "script.h"
#include "palette.h"
#include "tv.h"
#include "random.h"
#include "string_util.h"
#include "field_player_avatar.h"
#include "event_object_movement.h"
#include "constants/species.h"
#include "constants/maps.h"
#include "constants/flags.h"
#include "constants/tv.h"
#include "constants/characters.h"
#include "sl_test.h"
#include "genetics.h"
#include "roamer.h"
#include "sprite.h"

struct SlTestMailbox
{
    u32 magic;        // 0
    u16 scenario;     // 4  written by the runner
    u8 phase;         // 6  1 = scenario set up, 2 = field idle at the start position
    u8 scriptOn;      // 7
    u8 locked;        // 8
    u8 fieldCb;       // 9  1 = CB2_Overworld / CB1_Overworld
    u8 follower;      // 10 1 = a follower object exists
    u8 pad;           // 11
    s16 px, py;       // 12, 14
    u32 frame;        // 16
    u32 sc[3];        // 20 script ptr, native ptr, mode | depth<<8 | status<<16
    u32 unit[4];      // 32 scenario 0x400 unit results
    u8 mapGroup, mapNum, partyCount, pad2;   // 48
    u16 species[3];   // 52
    u8 level[3];      // 58
    u8 gender[3];     // 61
};
void SlTest_ScriptState(u32 *out);
EWRAM_DATA struct SlTestMailbox gSlTest = {0};

// scenario: low byte = TV show kind to put on the air (0 = none), bit 8 = no party (no follower)
static void PutShow(u8 kind)
{
    TVShow *show = &gSaveBlock1Ptr->tvShows[0];
    static const u8 who[] = _("RIVAL");
    memset(show, 0, sizeof(*show));
    if (kind == 0)
        return;
    show->common.kind = kind;
    show->common.active = TRUE;
    // fill every byte after kind/active with a plausible small value so names and species read as real ones
    {
        u8 *b = (u8 *)show;
        u32 i;
        for (i = 2; i < sizeof(*show); i++)
            b[i] = 0;
    }
    (void)who;
}


// scenario bit 0x400: fix checks (legendary perfect IVs, roamer genome, follower palette slots)
static EWRAM_DATA u16 sUnitTimer = 0;
static void MakeBlack(struct Pokemon *mon)
{
    struct Genome g;
    GetBoxMonGenome(&mon->box, &g);
    g.coat[COAT_LOCUS_C] = 0; g.coat[COAT_LOCUS_E] = 0; g.coat[COAT_LOCUS_A] = 3;
    SetBoxMonGenome(&mon->box, &g);
}
static void UnitNewGame(void)
{
    struct Pokemon *mon = &gEnemyParty[0];
    u32 i, s, minPerfect = 6, same;
    struct Genome g1, g2;
    for (i = 0; i < 20; i++)
    {
        u32 n = 0;
        CreateMonWithIVs(mon, SPECIES_REGIROCK, 40, Random32(), OTID_STRUCT_PLAYER_ID, USE_RANDOM_IVS);
        for (s = 0; s < NUM_STATS; s++) n += GetMonData(mon, MON_DATA_HP_IV + s) == MAX_PER_STAT_IVS;
        if (n < minPerfect) minPerfect = n;
    }
    gSlTest.unit[0] = minPerfect;
    gSaveBlock1Ptr->roamer[0].species = SPECIES_LATIOS; gSaveBlock1Ptr->roamer[0].level = 40;
    gSaveBlock1Ptr->roamer[0].ivs = Random32() & 0x3FFFFFFF; gSaveBlock1Ptr->roamer[0].personality = Random32();
    gSaveBlock1Ptr->roamer[0].hp = 100; gSaveBlock1Ptr->roamer[0].active = TRUE; gSaveBlock1Ptr->roamer[0].shiny = FALSE;
    same = 1;
    for (i = 0; i < 8; i++)
    {
        CreateRoamerMonInstance(0);
        GetBoxMonGenome(&gEnemyParty[0].box, i == 0 ? &g1 : &g2);
        if (i > 0 && (GenomeCoat(&g1) != GenomeCoat(&g2) || GenomeHueShift(&g1) != GenomeHueShift(&g2) || GenomeIsShiny(&g1) != GenomeIsShiny(&g2)
                   || GenomeDisplayScale(&g1, gSaveBlock1Ptr->roamer[0].personality, 512) != GenomeDisplayScale(&g2, gSaveBlock1Ptr->roamer[0].personality, 512)))
            same = 0;
        if (i == 0) memset(&g2, 0, sizeof(g2));
        for (s = 0; s < NUM_STATS; s++)
        {
            static EWRAM_DATA u8 sFirstIv[NUM_STATS] = {0};
            u8 iv = GetMonData(&gEnemyParty[0], MON_DATA_HP_IV + s);
            if (i == 0) sFirstIv[s] = iv; else if (iv != sFirstIv[s]) same |= 4;
        }
    }
    gSlTest.unit[1] = same;
    // two black leads to swap between
    CreateMon(&gPlayerParty[1], SPECIES_TREECKO, 20, 0x87654321, OTID_STRUCT_PLAYER_ID);
    CalculateMonStats(&gPlayerParty[1]);
    { u16 hp = GetMonData(&gPlayerParty[1], MON_DATA_MAX_HP); SetMonData(&gPlayerParty[1], MON_DATA_HP, &hp); }
    MakeBlack(&gPlayerParty[0]);
    MakeBlack(&gPlayerParty[1]);
    CalculatePlayerPartyCount();
}
static void UnitFrame(void)
{
    if (gSlTest.phase != 2 || GetFollowerObject() == NULL)
        return;
    sUnitTimer++;
    if (sUnitTimer == 60)
        gSlTest.unit[2] = CountFreeSpritePalettes();
    if (sUnitTimer > 60 && sUnitTimer <= 60 + 6 * 40 && (sUnitTimer % 40) == 0)
    {
        struct Pokemon tmp = gPlayerParty[0]; gPlayerParty[0] = gPlayerParty[1]; gPlayerParty[1] = tmp;
        UpdateFollowingPokemon();
    }
    if (sUnitTimer == 60 + 6 * 40 + 60)
        gSlTest.unit[3] = CountFreeSpritePalettes() | 0x100;   // 0x100 = measured
}

void SlTest_NewGame(void)
{
    static const u8 name[] = _("SL");
    struct Pokemon *mon = &gPlayerParty[0];
    int i;
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++) gSaveBlock2Ptr->playerName[i] = (i < 2) ? name[i] : EOS;
    gSaveBlock2Ptr->playerGender = MALE;
    gSaveBlock2Ptr->optionsTextSpeed = OPTIONS_TEXT_SPEED_FAST;
    FlagSet(FLAG_SYS_POKEMON_GET);
    if (!(gSlTest.scenario & 0x100))
    {
        u16 hp;
        CreateMon(mon, SPECIES_MUDKIP, 20, 0x12345678, OTID_STRUCT_PLAYER_ID);
        CalculateMonStats(mon);
        hp = GetMonData(mon, MON_DATA_MAX_HP);
        SetMonData(mon, MON_DATA_HP, &hp);
        CalculatePlayerPartyCount();
    }
    if (gSlTest.scenario & 0x400)
        UnitNewGame();
    FlagSet(FLAG_SYS_TV_START);
    FlagClear(FLAG_SYS_TV_WATCH);
    PutShow(gSlTest.scenario & 0xFF);
    gSlTest.phase = 1;
    SetWarpDestination(MAP_GROUP(MAP_MAUVILLE_CITY_HOUSE1), MAP_NUM(MAP_MAUVILLE_CITY_HOUSE1), WARP_ID_NONE, 2, 2);
    WarpIntoMap();
}

void SlTest_Frame(void)
{
    gSlTest.magic = 0x54534C53;   // "SLST"
    gSlTest.frame++;
    SlTest_ScriptState(gSlTest.sc);
    gSlTest.scriptOn = ScriptContext_IsEnabled();
    gSlTest.locked = ArePlayerFieldControlsLocked();
    gSlTest.fieldCb = gMain.callback2 == CB2_Overworld && gMain.callback1 == CB1_Overworld;
    gSlTest.follower = GetFollowerObject() != NULL;
    if (gSaveBlock1Ptr)
    {
        u32 i;
        gSlTest.mapGroup = gSaveBlock1Ptr->location.mapGroup;
        gSlTest.mapNum = gSaveBlock1Ptr->location.mapNum;
        gSlTest.partyCount = gPlayerPartyCount;
        for (i = 0; i < 3; i++)
        {
            gSlTest.species[i] = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES);
            gSlTest.level[i] = GetMonData(&gPlayerParty[i], MON_DATA_LEVEL);
            gSlTest.gender[i] = GetMonGender(&gPlayerParty[i]);
        }
    }
    gSlTest.pad = (GetFirstLiveMon() != NULL) | (gPlayerPartyCount << 1);
    if (gSlTest.phase == 2 && !gSlTest.follower && gSlTest.fieldCb && !(gSlTest.scenario & 0x100) && (gSlTest.frame & 63) == 0)
        UpdateFollowingPokemon();
    if (gSaveBlock1Ptr)
    {
        gSlTest.px = gSaveBlock1Ptr->pos.x;
        gSlTest.py = gSaveBlock1Ptr->pos.y;
    }
    if (gSlTest.scenario & 0x400)
        UnitFrame();
    if (gSlTest.phase == 1 && gSlTest.fieldCb && !gSlTest.locked && !gPaletteFade.active)
        gSlTest.phase = 2;
}
#endif
