// Emerald Evolved: the Breeder start. Prof. Birch asks at the end of his intro whether you want to skip ahead; if you
// do, the game begins outside the Pokémon Day Care on Route 117 with the story at "just arrived in Mauville" (the same
// flag set the softlock sweep tested), you pick your starter there, and you get an Eevee pair to breed straight away.
#include "global.h"
#include "breeder_start.h"
#include "main.h"
#include "event_data.h"
#include "field_screen_effect.h"
#include "fieldmap.h"
#include "item.h"
#include "money.h"
#include "overworld.h"
#include "palette.h"
#include "pokedex.h"
#include "pokemon.h"
#include "random.h"
#include "rtc.h"
#include "script.h"
#include "script_pokemon_util.h"
#include "starter_choose.h"
#include "task.h"
#include "constants/flags.h"
#include "constants/heal_locations.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/species.h"
#include "constants/trainers.h"
#include "constants/vars.h"
#include "breeder_midgame.h"

extern const u8 EventScript_BreederStart[];

EWRAM_DATA bool8 gBreederStart = FALSE;
EWRAM_DATA bool8 gBreederChoosingStarter = FALSE;

#define BREEDER_STARTER_LEVEL 15
#define BREEDER_EEVEE_LEVEL   12

static void SetStoryToMauville(void)
{
    u32 i;
    for (i = 0; i < ARRAY_COUNT(sBreederMidFlagsSet); i++)
        FlagSet(sBreederMidFlagsSet[i]);
    for (i = 0; i < ARRAY_COUNT(sBreederMidFlagsClear); i++)
        FlagClear(sBreederMidFlagsClear[i]);
    for (i = 0; i < ARRAY_COUNT(sBreederMidVars); i++)
        VarSet(sBreederMidVars[i][0], sBreederMidVars[i][1]);
    FlagSet(TRAINER_FLAGS_START + TRAINER_ROXANNE_1);
    FlagSet(TRAINER_FLAGS_START + TRAINER_BRAWLY_1);
    FlagSet(FLAG_VISITED_MAUVILLE_CITY);
    FlagSet(FLAG_RECEIVED_BIKE);
    FlagSet(FLAG_SET_WALL_CLOCK);
}

// called from CB2_NewGame (after NewGameInitData) when the player said yes in Birch's intro
void BreederStart_NewGame(void)
{
    SetStoryToMauville();
    // the wall clock is normally set in your bedroom: start it at the console's current time instead
    RtcCalcLocalTime();
    RtcInitLocalTimeOffset(gLocalTime.hours, gLocalTime.minutes);
    SetMoney(&gSaveBlock1Ptr->money, 5000);
    AddBagItem(ITEM_MACH_BIKE, 1);
    AddBagItem(ITEM_POKE_BALL, 20);
    AddBagItem(ITEM_POTION, 5);
    AddBagItem(ITEM_HM01, 1);   // FLAG_RECEIVED_HM_CUT is part of the story state
    AddBagItem(ITEM_HM05, 1);   // FLAG_RECEIVED_HM_FLASH too
    SetLastHealLocationWarp(HEAL_LOCATION_MAUVILLE_CITY);
    SetWarpDestination(MAP_GROUP(MAP_ROUTE117), MAP_NUM(MAP_ROUTE117), WARP_ID_NONE, 51, 6);   // outside the Day Care door
    WarpIntoMap();
}

static void Task_BreederStartScript(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        DestroyTask(taskId);
        ScriptContext_SetupScript(EventScript_BreederStart);
    }
}

void FieldCB_BreederStart(void)
{
    Overworld_PlaySpecialMapMusic();
    FadeInFromBlack();
    LockPlayerFieldControls();
    CreateTask(Task_BreederStartScript, 10);
}

static void GiveEevee(bool32 female)
{
    struct Pokemon mon;
    // Eevee is 7 male : 1 female; the personality's low byte decides it (below 31 = female)
    u32 personality = (Random32() & 0xFFFFFF00) | (female ? (Random() % 31) : 31 + (Random() % 225));
    CreateMon(&mon, SPECIES_EEVEE, BREEDER_EEVEE_LEVEL, personality, OTID_STRUCT_PLAYER_ID);
    GiveCapturedMonToPlayer(&mon);
    GetSetPokedexFlag(SpeciesToNationalPokedexNum(SPECIES_EEVEE), FLAG_SET_SEEN);
    GetSetPokedexFlag(SpeciesToNationalPokedexNum(SPECIES_EEVEE), FLAG_SET_CAUGHT);
}

static void CB2_BreederGiveStarter(void)
{
    gBreederChoosingStarter = FALSE;
    *GetVarPointer(VAR_STARTER_MON) = gSpecialVar_Result;
    ScriptGiveMon(GetStarterPokemon(gSpecialVar_Result), BREEDER_STARTER_LEVEL, ITEM_NONE);
    GiveEevee(FALSE);
    GiveEevee(TRUE);
    SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

// special: the normal starter bag screen, without Route 101's first battle
void BreederChooseStarter(void)
{
    gBreederChoosingStarter = TRUE;
    SetMainCallback2(CB2_ChooseStarter);
    gMain.savedCallback = CB2_BreederGiveStarter;
}
