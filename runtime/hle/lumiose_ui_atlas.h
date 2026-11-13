/* Builds the mod's icon atlas from PNGs extracted locally from the player's game.
   This file contains mappings only, not artwork. */
#pragma once
#include <SDL3/SDL_surface.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lumiose_icon_index.h"
#include "lumiose_item_index.h"

enum { LUMIOSE_ICON_COLS = 21, LUMIOSE_ICON_ROWS = 19, LUMIOSE_ICON_CELL = 48,
       LUMIOSE_ATLAS_WIDTH = 1024, LUMIOSE_ATLAS_HEIGHT = 2048,
       LUMIOSE_ITEM_Y = 912, LUMIOSE_ITEM_CELL = 48, LUMIOSE_ITEM_COLS = 21 };

static int lumiose_icon_model(int species) {
    static const short kalos[72] = {
        720,721,722,717,718,719,723,724,725,711,712,753,754,755,706,707,708,704,705,713,714,715,728,729,
        752,730,701,733,734,744,745,746,758,759,742,743,726,727,747,748,709,710,756,757,731,732,737,738,
        739,740,767,761,741,766,763,764,765,760,702,703,735,736,749,751,716,762,768,769,770,772,774,773};
    static const short later[][2] = {
        {739,859},{740,860},{767,866},{768,867},{769,822},{770,823},{778,819},{780,856},{801,882},
        {802,883},{807,888},{808,891},{809,892},{821,918},{822,919},{823,920},{827,901},{828,902},
        {848,921},{849,922},{850,923},{851,923},{852,964},{853,965},{876,942},{877,941},{920,1002},
        {926,1074},{927,1075},{931,1064},{932,1067},{933,1068},{934,1069},{935,1107},{936,1108},
        {937,1109},{942,1076},{943,1077},{951,1042},{952,1043},{957,1104},{958,1105},{959,1106},
        {965,1072},{966,1073},{967,1057},{969,1070},{970,1071},{971,1028},{972,1029},{973,1065},
        {977,1035},{978,1056},{979,1114},{996,1053},{997,1054},{998,1055},{999,1080},{1000,1081}};
    if (species < 650) return species;
    if (species <= 721) return kalos[species - 650];
    for (unsigned i = 0; i < sizeof later / sizeof later[0]; ++i)
        if (later[i][0] == species) return later[i][1];
    return -1;
}

// Return the same locally extracted image used by an atlas cell, at its
// original 160 px resolution for the large selected-Pokemon card.
static SDL_Surface *lumiose_build_mon_detail(const char *directory, unsigned cell, bool shiny) {
    if (!directory || !*directory || cell >= LUMIOSE_ICON_COLS * LUMIOSE_ICON_ROWS) return NULL;
    for (uint32_t i = 0; i < kIconIndexCount; ++i) {
        const struct IconIndex *entry = &kIconIndex[i];
        if (entry->cell != cell) continue;
        const int model = lumiose_icon_model(entry->species);
        if (model < 0) continue;
        const int forms[] = {entry->form, 11 + entry->form, 0, 11};
        for (unsigned f = 0; f < sizeof forms / sizeof forms[0]; ++f) {
            char path[4096];
            int n = snprintf(path, sizeof path, "%s/pm%04d_%02d_00_00_%d.png", directory, model, forms[f], shiny ? 1 : 0);
            if (n < 0 || (size_t)n >= sizeof path) break;
            SDL_Surface *source = SDL_LoadPNG(path);
            if (!source) continue;
            SDL_Surface *image = SDL_CreateSurface(160, 160, SDL_PIXELFORMAT_RGBA32);
            if (image) {
                SDL_ClearSurface(image, 0, 0, 0, 0);
                SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
                if (!SDL_BlitSurfaceScaled(source, NULL, image, NULL, SDL_SCALEMODE_LINEAR)) {
                    SDL_DestroySurface(image);
                    image = NULL;
                }
            }
            SDL_DestroySurface(source);
            return image;
        }
    }
    return NULL;
}

static SDL_Surface *lumiose_build_icon_atlas(const char *directory) {
    if (!directory || !*directory) return NULL;
    SDL_Surface *atlas = SDL_CreateSurface(LUMIOSE_ATLAS_WIDTH,
                                           LUMIOSE_ATLAS_HEIGHT, SDL_PIXELFORMAT_RGBA32);
    if (!atlas) return NULL;
    SDL_ClearSurface(atlas, 0, 0, 0, 0);
    bool done[LUMIOSE_ICON_COLS * LUMIOSE_ICON_ROWS] = {false};
    unsigned count = 0;
    for (uint32_t i = 0; i < kIconIndexCount; ++i) {
        const struct IconIndex *entry = &kIconIndex[i];
        if (entry->cell >= LUMIOSE_ICON_COLS * LUMIOSE_ICON_ROWS || done[entry->cell]) continue;
        const int model = lumiose_icon_model(entry->species);
        if (model < 0) continue;
        const int forms[] = {entry->form, 11 + entry->form, 0, 11};
        SDL_Surface *icon = NULL;
        char path[4096];
        for (unsigned f = 0; f < sizeof forms / sizeof forms[0] && !icon; ++f) {
            const int n = snprintf(path, sizeof path, "%s/pm%04d_%02d_00_00_0.png", directory, model, forms[f]);
            if (n < 0 || (size_t)n >= sizeof path) break;
            icon = SDL_LoadPNG(path);
        }
        if (!icon) continue;
        SDL_Rect dst = {(entry->cell % LUMIOSE_ICON_COLS) * LUMIOSE_ICON_CELL,
                        (entry->cell / LUMIOSE_ICON_COLS) * LUMIOSE_ICON_CELL,
                        LUMIOSE_ICON_CELL, LUMIOSE_ICON_CELL};
        SDL_SetSurfaceBlendMode(icon, SDL_BLENDMODE_NONE);
        const bool ok = SDL_BlitSurfaceScaled(icon, NULL, atlas, &dst, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(icon);
        if (ok) { done[entry->cell] = true; ++count; }
    }
    for (unsigned i = 0; i < kItemIconCount && i < 399; ++i) {
        char path[4096];
        int n = snprintf(path, sizeof path, "%s/item_%04u.png", directory, (unsigned)kItemIconIds[i]);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        SDL_Surface *icon = SDL_LoadPNG(path);
        if (!icon) continue;
        SDL_Rect dst = {(int)(i % LUMIOSE_ITEM_COLS) * LUMIOSE_ITEM_CELL,
                        LUMIOSE_ITEM_Y + (int)(i / LUMIOSE_ITEM_COLS) * LUMIOSE_ITEM_CELL,
                        LUMIOSE_ITEM_CELL, LUMIOSE_ITEM_CELL};
        SDL_SetSurfaceBlendMode(icon, SDL_BLENDMODE_NONE);
        if (SDL_BlitSurfaceScaled(icon, NULL, atlas, &dst, SDL_SCALEMODE_LINEAR)) ++count;
        SDL_DestroySurface(icon);
    }
    if (!count) { SDL_DestroySurface(atlas); return NULL; }
    return atlas;
}

// Preserve the game's original item resolution for the selected detail card.
static SDL_Surface *lumiose_build_item_detail(const char *directory, unsigned item) {
    if (!directory || !*directory || item > 9999) return NULL;
    char path[4096];
    const int n = snprintf(path, sizeof path, "%s/item_%04u.png", directory, item);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    SDL_Surface *source = SDL_LoadPNG(path);
    SDL_Surface *image = source ? SDL_CreateSurface(160, 160, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (image) {
        SDL_ClearSurface(image, 0, 0, 0, 0);
        SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
        if (!SDL_BlitSurfaceScaled(source, NULL, image, NULL, SDL_SCALEMODE_LINEAR)) {
            SDL_DestroySurface(image);
            image = NULL;
        }
    }
    if (source) SDL_DestroySurface(source);
    return image;
}
