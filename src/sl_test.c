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
    gSlTest.pad = (GetFirstLiveMon() != NULL) | (gPlayerPartyCount << 1);
    if (gSlTest.phase == 2 && !gSlTest.follower && gSlTest.fieldCb && !(gSlTest.scenario & 0x100) && (gSlTest.frame & 63) == 0)
        UpdateFollowingPokemon();
    if (gSaveBlock1Ptr)
    {
        gSlTest.px = gSaveBlock1Ptr->pos.x;
        gSlTest.py = gSaveBlock1Ptr->pos.y;
    }
    if (gSlTest.phase == 1 && gSlTest.fieldCb && !gSlTest.locked && !gPaletteFade.active)
        gSlTest.phase = 2;
}
#endif
