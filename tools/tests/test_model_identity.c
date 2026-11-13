#include <assert.h>
#include "../../runtime/hle/lumiose_model_protocol.h"
int main(void) {
    LumioseModelIdentity m;
    assert(!lumiose_model_decode(LUMIOSE_MODEL_V2_TYPE, &m));
    assert(!lumiose_model_decode(UINT64_C(0x4c580001), &m));
    assert(!lumiose_model_decode(UINT64_C(0x14d000001), &m));
    assert(!lumiose_model_decode(UINT64_C(0x4d100001), &m));
    assert(!lumiose_model_decode(LUMIOSE_MODEL_V2_TYPE | 1 | (3u<<17), &m));
    for (unsigned form=0;form<64;form++) for(unsigned gender=0;gender<3;gender++) for(unsigned shiny=0;shiny<2;shiny++) {
        uint64_t t=LUMIOSE_MODEL_V2_TYPE | 670 | (form<<11) | (gender<<17) | (shiny<<19);
        assert(lumiose_model_decode(t,&m));
        assert(m.species==670 && m.form==form && m.gender==gender && m.shiny==shiny);
        assert(m.key==(unsigned)(t&0xfffff));
    }
    assert(lumiose_model_decode(LUMIOSE_MODEL_V2_TYPE | 2047 | (63u<<11) | (2u<<17) | (1u<<19),&m));
    assert(!lumiose_model_decode(LUMIOSE_MODEL_V2_TYPE|1,0));
    return 0;
}
