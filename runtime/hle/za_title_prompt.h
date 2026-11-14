#ifndef ZA_TITLE_PROMPT_H
#define ZA_TITLE_PROMPT_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
// Verified 2.0.2 title_menu_00 grptext_title_key_guide_00 resource geometry.
// The mod replaces its pixels; identify the native resource role independently
// of those pixels and of the downloaded mod's build ID.
static inline bool za_title_prompt_shape(const uint8_t build[8],unsigned width,unsigned height,unsigned format,unsigned target,unsigned samples){
    static const uint8_t za202[8]={0xb1,0xf1,0x2f,0xd9,0x19,0xea,0xe8,0x6a};
    return width==640&&height==64&&format==0x4b&&target==1&&samples<=1&&!memcmp(build,za202,8);
}
#endif
