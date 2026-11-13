#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/hle/lumiose_ui_atlas.h"

int main(int argc, char **argv) {
    if (argc != 2 || !SDL_Init(0)) return 2;
    if (lumiose_build_icon_atlas(NULL)) return 3;
    SDL_Surface *atlas = lumiose_build_icon_atlas(argv[1]);
    if (!atlas || atlas->w != 1024 || atlas->h != 2048) return 4;
    SDL_Surface *detail = lumiose_build_item_detail(argv[1], 4);
    if (!detail || detail->w != 160 || detail->h != 160 || lumiose_build_item_detail(argv[1], 9999)) return 7;
    SDL_Surface *mon = lumiose_build_mon_detail(argv[1], 0, false);
    if (!mon || mon->w != 160 || mon->h != 160 || lumiose_build_mon_detail(argv[1], 9999, false)) return 12;
    char mon_path[4096];
    if (snprintf(mon_path, sizeof mon_path, "%s/pm0001_00_00_00_0.png", argv[1]) >= (int)sizeof mon_path) return 13;
    SDL_Surface *mon_loaded = SDL_LoadPNG(mon_path);
    SDL_Surface *mon_original = mon_loaded ? SDL_ConvertSurface(mon_loaded, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (!mon_original || mon_original->w != 160 || mon_original->h != 160) return 14;
    for (int y = 0; y < 160; ++y)
        if (memcmp((const unsigned char *)mon->pixels + y * mon->pitch,
                   (const unsigned char *)mon_original->pixels + y * mon_original->pitch, 160 * 4)) return 15;
    SDL_DestroySurface(mon_original);
    SDL_DestroySurface(mon_loaded);
    SDL_DestroySurface(mon);
    if (kItemIconCount > 399) return 8; // The detail slot starts after 19 item rows.
    char path[4096];
    if (snprintf(path, sizeof path, "%s/item_0004.png", argv[1]) >= (int)sizeof path) return 9;
    SDL_Surface *loaded = SDL_LoadPNG(path);
    SDL_Surface *original = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (!original || original->w != 160 || original->h != 160) return 10;
    for (int y = 0; y < 160; ++y)
        if (memcmp((const unsigned char *)detail->pixels + y * detail->pitch,
                   (const unsigned char *)original->pixels + y * original->pitch, 160 * 4)) return 11;
    SDL_DestroySurface(original);
    SDL_DestroySurface(loaded);
    unsigned detail_visible = 0;
    const unsigned char *detail_pixels = detail->pixels;
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 160; ++x)
            if (detail_pixels[y * detail->pitch + x * 4 + 3]) ++detail_visible;
    SDL_DestroySurface(detail);
    unsigned visible = 0;
    const unsigned char *pixels = atlas->pixels;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            if (pixels[y * atlas->pitch + x * 4 + 3]) ++visible;
    unsigned slot = 0, item_visible = 0;
    while (slot < kItemIconCount && kItemIconIds[slot] != 4) ++slot;
    if (slot == kItemIconCount) return 6;
    int left = (slot % 21) * 48, top = 912 + (slot / 21) * 48;
    for (int y = top; y < top + 48; ++y)
        for (int x = left; x < left + 48; ++x)
            if (pixels[y * atlas->pitch + x * 4 + 3]) ++item_visible;
    SDL_DestroySurface(atlas);
    SDL_Quit();
    if (visible < 20 || item_visible < 20 || detail_visible < 1000) return 5;
    printf("UI atlas and detail pictures match the supplied local fixture pixels.\n");
    return 0;
}
