#ifndef GUARD_HYBRID_SPRITE_H
#define GUARD_HYBRID_SPRITE_H

// Cross-breed hybrid sprites (GAME_SPEC feature 3). A hybrid is drawn as the drawn species' real sprite (the mother's
// line), its body ramp recoloured with the colour species' ramp, plus real body parts transplanted pixel-exact from the
// part species' sprites, merged to the 15 colours a sprite palette holds. The table is tools/hybrid/hybrid_table.py,
// baked into src/data/hybrid_sprites.h; tools/hybrid/hybrid_lib.py is the reference implementation of the same steps.
//
// Hooked into LoadSpecialPokePicIsEgg (front/back pics) and the species+personality palette getters, so every screen that
// draws a Pokemon from species + personality (battle, summary, PC, trade, evolution...) draws the hybrid.

#define HYBRID_MAX_PARTS        2
#define HYBRID_MAX_PART_COLOURS 32

enum { HYBRID_VIEW_FRONT_1, HYBRID_VIEW_FRONT_2, HYBRID_VIEW_BACK, HYBRID_VIEW_COUNT };

struct HybridLook
{
    u16 colourSpecies;                      // whose body colours (the father); SPECIES_NONE = keep the body's own
    u16 partSpecies[HYBRID_MAX_PARTS];      // whose body parts, in draw order; SPECIES_NONE = none
};

struct HybridRamp
{
    u16 species;
    u8 count;
    u8 idx[6];                              // body ramp, light -> dark, as indices of the species' palette
};

struct HybridRampMap                        // an owner-picked map: body palette index -> colour species palette index
{
    u16 bodySpecies;
    u16 colourSpecies;
    u8 count;
    u8 bodyIdx[8];
    u8 colourIdx[8];
};

struct HybridLayer
{
    u16 bodySpecies;
    u16 partSpecies;
    u8 view;
    bool8 behind;                           // drawn behind the body (else in front of it)
    u8 x, y, w, h;                          // in the body's own 64x64 frame
    u8 colourCount;
    const u16 *colours;                     // RGB555
    const u8 *pixels;                       // w * h, 0 = transparent, k = colours[k - 1]
};

struct BoxPokemon;

// The hybrid look of an owned Pokemon. FALSE = not a hybrid. Lineage storage belongs to cross-species eggs (feature 2);
// until it lands this only knows test looks (EVO_TEST builds).
bool32 GetBoxMonHybridLook(const struct BoxPokemon *boxMon, struct HybridLook *look);

// Composite a decompressed front (64x128, both anim frames) or back (64x64) pic in place. No-op for non-hybrids.
void HybridApplyPic(void *pic, u16 species, u32 personality, bool32 isFrontPic);
// The hybrid's palette for `base` (the species' normal or shiny palette), or `base` itself for non-hybrids.
const u16 *HybridPalette(u16 species, u32 personality, bool32 isShiny, const u16 *base);
const u16 *HybridBoxMonPalette(const struct BoxPokemon *boxMon, u16 species, bool32 isShiny, const u16 *base);

#if EVO_TEST
void HybridSetTestLook(u32 personality, const struct HybridLook *look);   // look NULL = clear all
#endif

#endif // GUARD_HYBRID_SPRITE_H
