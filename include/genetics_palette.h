#ifndef GUARD_GENETICS_PALETTE_H
#define GUARD_GENETICS_PALETTE_H
#include "genetics.h"
struct BoxPokemon;
// Recolours one 16-colour palette from the genome: coat (albino/red/black, epistatic) then the hue gene. Index 0
// (transparent) and outline/white colours are left alone; shading ramps are kept (luma preserved).
void ApplyGenesTo16(u16 *pal, u8 coat, s32 hueShift);
// Same, with per-entry weights (pixel counts) for picking the dominant body-hue cluster the hue gene recolours.
void ApplyGenesTo16Weighted(u16 *pal, u8 coat, s32 hueShift, const u16 *weights);
// Mon icons: gives a just-created icon sprite its own recoloured palette (party, PC boxes, Day Care, anywhere icons
// are built from species + personality). The hint names a Pokémon that is in none of those places (the PC's held mon).
void SetMonIconGenePalette(u8 spriteId, u16 species, u32 personality, u8 basePal, const u8 *iconTiles);
void SetMonIconGenomeHint(const struct BoxPokemon *boxMon);
// Genetic palette of the player's / Day Care / opponent Pokémon with this personality, else `base`.
const u16 *GeneticPaletteForPersonality(u32 personality, const u16 *base);
// For mon-aware palette getters: returns `base`, or a recoloured copy in a small EWRAM ring (4 slots).
const u16 *GeneticMonPalette(const struct BoxPokemon *boxMon, const u16 *base);
#endif
