#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include "../hle.h"
#include "../settings.h"
#include "../input_bind.h"
#include "za_key_glyphs.h"

// Combined game fonts are generated from the player's own assets. The manifest maps each
// original/static font to its combined version, without changing the packed game archive.
typedef struct { uint64_t hash, target; } FontMap;
typedef struct { uint64_t id, addr; } FontFile;
static FontMap maps[64];
static FontFile files[8];
static int nmaps, nfiles, initialized;
static atomic_bool ready, contexts_ready, bag_context_ready, keybank_ready;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static uint64_t font_hash(uint64_t addr) {
    if (addr < 0x7000000000ull || addr > 0x77ffffff00ull) return 0;
    const unsigned char *p = (const unsigned char *)(uintptr_t)addr;
    if (memcmp(p, "OTTO", 4)) return 0;
    unsigned n = (unsigned)p[4]<<8 | p[5];
    if (!n || n > 128) return 0;
    size_t end = 12 + 16*n;
    for (unsigned i=0; i<n; i++) {
        const unsigned char *t = p+12+16*i;
        uint32_t off=be32(t+8), len=be32(t+12);
        if (off > 4*1024*1024 || len > 4*1024*1024 || off+len > 4*1024*1024) return 0;
        size_t e = (off+len+3u)&~3u;
        if (e>end) end=e;
    }
    if (addr + end > 0x7800000000ull) return 0;
    uint64_t h=0xcbf29ce484222645ull;
    for (size_t i=0; i<end; i++) h=(h^p[i])*0x100000001b3ull;
    return h;
}
static void init(void) {
    initialized=1;
#ifdef __APPLE__
    return;
#endif
    const char *disable=getenv("LUMIOSE_LIVE_PROMPTS");
    if ((g_settings.prompts != PROMPTS_AUTO && g_settings.prompts != PROMPTS_KEYBOARD) || (disable && !strcmp(disable,"0"))) return;
    FILE *f=fopen("textures/prompts/live/manifest.txt","r");
    if (!f) return;
    unsigned long long h,id; char line[100], tail; bool contexts = false, bag = false;
    while (fgets(line,sizeof line,f) && nmaps<64) {
        if (!strncmp(line, "# keyboard-context-v1", 21)) { contexts = true; continue; }
        if (!strncmp(line, "# keyboard-context-v2", 21)) { contexts = bag = true; continue; }
        if (!strncmp(line, "# keyboard-keybank-v1", 20)) { atomic_store(&keybank_ready,true); continue; }
        if (sscanf(line,"%16llx %16llx.ttf %c",&h,&id,&tail)==2)
            maps[nmaps++]=(FontMap){(uint64_t)h,(uint64_t)id};
    }
    fclose(f);
    atomic_store(&contexts_ready, contexts);
    atomic_store(&bag_context_ready, bag);
    LOG("[prompts] context sets %s\n", contexts ? "available" : "not available");
}
uint64_t rt_za_prompt_font(uint64_t addr) {
    pthread_mutex_lock(&lock);
    if (!initialized) init();
    uint64_t h=nmaps ? font_hash(addr) : 0, target=0, result=addr;
    for (int i=0; i<nmaps; i++) if (maps[i].hash==h) { target=maps[i].target; break; }
    if (target) {
        int i=0; while (i<nfiles && files[i].id!=target) i++;
        if (i==nfiles && nfiles<8) {
            char path[100]; snprintf(path,sizeof path,"textures/prompts/live/%016llx.ttf",(unsigned long long)target);
            FILE *f=fopen(path,"rb");
            if (f) {
                if (!fseek(f,0,SEEK_END)) {
                    long len=ftell(f); rewind(f);
                    if (len>=12 && len<=4*1024*1024) {
                        unsigned char *buf=malloc((size_t)len);
                        if (buf && fread(buf,1,(size_t)len,f)==(size_t)len && !memcmp(buf,"OTTO",4)) {
                            void *dst=hle_alloc((uint64_t)len); memcpy(dst,buf,(size_t)len);
                            files[nfiles++]=(FontFile){target,(uint64_t)(uintptr_t)dst};
                            LOG("[prompts] loaded live font %016llx\n",(unsigned long long)target);
                        }
                        free(buf);
                    }
                }
                fclose(f);
            }
        }
        if (i<nfiles) result=files[i].addr;
        if (nfiles>=2) atomic_store(&ready,true);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
int rt_za_live_prompt_style(void) {
    if (!atomic_load(&ready)) return -1;
    int style = g_settings.prompts == PROMPTS_AUTO ? input_prompt_auto() : g_settings.prompts;
    if (style == PROMPTS_KEYBOARD && atomic_load(&contexts_ready)) {
        extern int input_keyboard_prompt_context(void);
        int context=input_keyboard_prompt_context();
        if(context==4&&!atomic_load(&bag_context_ready))context=1;
        if(context==7)context=1;
        return 100 + context;
    }
    return style;
}
uint32_t rt_za_prompt_code(uint32_t cp) {
    bool glyph=(cp>=0xe31d && cp<=0xe32a) || (cp>=0xe32e && cp<=0xe331) ||
        cp==0xe335 || cp==0xe336 || cp==0xe338 || cp==0xe339 || (cp>=0xe33c && cp<=0xe33f);
    if (!glyph || !atomic_load(&ready)) return cp;
    int style=g_settings.prompts == PROMPTS_AUTO ? input_prompt_auto() : g_settings.prompts;
    if (style == PROMPTS_KEYBOARD) {
        static const uint32_t bases[] = {0xe600, 0xe800, 0xe900, 0xea00, 0xeb00};
        extern int input_keyboard_prompt_context(void);
        int context = atomic_load(&contexts_ready) ? input_keyboard_prompt_context() : 0;
        if (context==4&&!atomic_load(&bag_context_ready))context=1;
        if (context < 0 || context > 7) context = 0;
        if(context==7&&(cp==0xe323||cp==0xe324))return ' ';
        if(atomic_load(&keybank_ready)){
            if(cp==0xe335||cp==0xe33c||cp==0xe33d)return ZA_KEY_GLYPH_BASE+523;
            if(cp==0xe336||cp==0xe33e||cp==0xe33f)return ZA_KEY_GLYPH_BASE+
                (g_settings.mouse_camera==MOUSE_CAM_OFF?524:g_settings.mouse_camera==MOUSE_CAM_HOLD?522:521);
            int slot=-1;
            switch(cp){
            case 0xe31d:slot=BIND_A;break;case 0xe31e:slot=BIND_B;break;
            case 0xe31f:slot=BIND_X;break;case 0xe320:slot=BIND_Y;break;
            case 0xe321:slot=BIND_L;break;case 0xe322:slot=BIND_R;break;
            case 0xe323:slot=BIND_ZL;break;case 0xe324:slot=BIND_ZR;break;
            case 0xe325:case 0xe32e:slot=BIND_UP;break;
            case 0xe326:case 0xe32f:slot=BIND_DOWN;break;
            case 0xe327:case 0xe330:slot=BIND_LEFT;break;
            case 0xe328:case 0xe331:slot=BIND_RIGHT;break;
            case 0xe329:slot=BIND_PLUS;break;case 0xe32a:slot=BIND_MINUS;break;
            case 0xe338:slot=BIND_LSTICK;break;case 0xe339:slot=BIND_RSTICK;break;
            }
            if(slot>=0)return ZA_KEY_GLYPH_BASE+bind_prompt_keyboard_token(slot,context);
        }
        return bases[context>4?0:context] + cp - 0xe31d;
    }
    return style==PROMPTS_XBOX ? 0xe700+cp-0xe31d : cp;
}
RT_ON_RESET(za_live_prompts_reset) {
    atomic_store(&ready,false); atomic_store(&contexts_ready,false); atomic_store(&bag_context_ready,false); atomic_store(&keybank_ready,false); initialized=nmaps=nfiles=0;
}
