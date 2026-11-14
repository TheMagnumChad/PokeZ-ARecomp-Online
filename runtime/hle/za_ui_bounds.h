#ifndef ZA_UI_BOUNDS_H
#define ZA_UI_BOUNDS_H

// The 2.0.2 game's pane cache and transforms. Call only from a live UI
// presenter callback; no guest object is retained after that callback.
typedef struct { float x, y, width, height; } ZaUiRect;
static bool za_ui_span(uint64_t p, uint64_t size) {
    return p >= 0x7400000000ull && p < 0x7800000000ull && size <= 0x7800000000ull - p;
}
static uint64_t za_ui_layout_pane(uint64_t layout, const char *name) {
    if (!za_ui_span(layout, 0x510)) return 0;
    uint64_t buckets = LD64(layout + 0x500), count = LD64(layout + 0x508);
    if (!count || count > 4096 || !za_ui_span(buckets, count * 8)) return 0;
    uint64_t hash = 0xcbf29ce484222645ull;
    size_t length = strlen(name);
    for (size_t i = 0; i < length; i++) hash = (hash ^ (unsigned char)name[i]) * 0x100000001b3ull;
    uint64_t pred = LD64(buckets + (hash % count) * 8);
    if (!za_ui_span(pred, 8)) return 0;
    uint64_t node = LD64(pred);
    for (unsigned depth = 0; depth < 128 && za_ui_span(node, 0x38); depth++, node = LD64(node)) {
        if (LD64(node + 8) % count != hash % count) return 0;
        if (LD64(node + 0x10) != hash || LD64(node + 0x20) != length) continue;
        uint64_t text = LD64(node + 0x18), pane = LD64(node + 0x30);
        if (!za_ui_span(text, length) || memcmp((void *)(uintptr_t)text, name, length) || !za_ui_span(pane, 0x60)) return 0;
        return pane;
    }
    return 0;
}
static uint64_t za_ui_pane(uint64_t view, const char *name) {
    uint64_t wrap=0;
    for(unsigned depth=0;depth<16&&za_ui_span(view,0xf0);depth++){
        uint64_t candidate=LD64(view+0xe8);
        if(za_ui_span(candidate,0x38)&&LD64(candidate)==0x7003bd4038ull){wrap=candidate;break;}
        uint64_t parent=LD64(view+0x88);if(parent==view)break;view=parent;
    }
    if(!wrap)return 0;
    uint64_t root=LD64(wrap+0x30);
    return za_ui_span(root,0x180)?za_ui_layout_pane(LD64(root+0x178),name):0;
}
static float za_ui_float(uint64_t p) {
    uint32_t bits = LD32(p); float value; memcpy(&value, &bits, sizeof value); return value;
}
static bool za_ui_logical_visible(uint64_t pane){
    if(!za_ui_span(pane,0x60))return false;
    for(unsigned depth=0;pane&&depth<32;depth++,pane=LD64(pane+0x18))
        if(!za_ui_span(pane,0x60)||!(LD8(pane+0x58)&1))return false;
    return !pane;
}
static bool za_ui_visible(uint64_t pane) {
    if(!za_ui_span(pane,0x60))return false;
    for(unsigned depth=0;pane&&depth<32;depth++,pane=LD64(pane+0x18))
        if(!za_ui_span(pane,0x60)||!(LD8(pane+0x58)&1)||!LD8(pane+0x5a))return false;
    return !pane;
}
static bool za_ui_rect(uint64_t pane, ZaUiRect *out) {
    if (!za_ui_span(pane, 0x60)) return false;
    float width = za_ui_float(pane + 0x50), height = za_ui_float(pane + 0x54);
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    float x[4] = {-width/2, width/2, width/2, -width/2};
    float y[4] = {-height/2, -height/2, height/2, height/2};
    unsigned base = LD8(pane + 0x5b), ax=base&3, ay=(base>>2)&3;
    if(ax==3 || ay==3) return false;
    for(unsigned i=0;i<4;i++){x[i]+=ax==1?width/2:ax==2?-width/2:0;y[i]+=ay==1?-height/2:ay==2?height/2:0;}
    for (unsigned depth = 0; pane && depth < 32; depth++) {
        if (!za_ui_span(pane, 0x60) || !(LD8(pane + 0x58) & 1) || !LD8(pane + 0x5a)) return false;
        float tx = za_ui_float(pane + 0x30), ty = za_ui_float(pane + 0x34);
        float sx = za_ui_float(pane + 0x48), sy = za_ui_float(pane + 0x4c), angle = za_ui_float(pane + 0x44);
        if (!isfinite(tx) || !isfinite(ty) || !isfinite(sx) || !isfinite(sy) || !isfinite(angle) ||
            fabsf(tx) > 10000 || fabsf(ty) > 10000 || fabsf(sx) > 100 || fabsf(sy) > 100) return false;
        float co = cosf(angle * 0.01745329252f), si = sinf(angle * 0.01745329252f);
        for (unsigned i = 0; i < 4; i++) {
            float px = x[i]*sx, py = y[i]*sy;
            x[i] = tx + px*co - py*si; y[i] = ty + px*si + py*co;
        }
        base=LD8(pane+0x5b); ax=(base>>4)&3; ay=(base>>6)&3;
        pane = LD64(pane + 0x18);
        if(ax==3 || ay==3) return false;
        if(pane && (ax || ay)) {
            if(!za_ui_span(pane,0x60)) return false;
            float pw=za_ui_float(pane+0x50),ph=za_ui_float(pane+0x54);
            if(!isfinite(pw)||!isfinite(ph))return false;
            for(unsigned i=0;i<4;i++){x[i]+=ax==1?-pw/2:ax==2?pw/2:0;y[i]+=ay==1?ph/2:ay==2?-ph/2:0;}
        }
    }
    if (pane) return false;
    float left=x[0],right=x[0],bottom=y[0],top=y[0];
    for(unsigned i=1;i<4;i++){left=fminf(left,x[i]);right=fmaxf(right,x[i]);bottom=fminf(bottom,y[i]);top=fmaxf(top,y[i]);}
    *out=(ZaUiRect){960+left,540-top,right-left,top-bottom};
    return out->width > 0 && out->height > 0 && isfinite(out->x) && isfinite(out->y);
}
typedef struct { ZaUiRect rect; unsigned index; bool enabled; } ZaUiCell;
static bool za_ui_key_text(uint64_t key, char *text, size_t capacity) {
    if (!capacity || !za_ui_span(key,24)) return false;
    uint64_t hash=0xcbf29ce484222645ull,str=LD64(key+8),len=LD64(key+16);
    if(!len||len>=capacity||!za_ui_span(str,len))return false;
    for(size_t i=0;i<len;i++){
        unsigned char ch=LD8(str+i);if(ch<32||ch>126)return false;
        text[i]=(char)ch;hash=(hash^ch)*0x100000001b3ull;
    }
    text[len]=0;return hash==LD64(key);
}
// Match Button::CanInput, including its owner/path permission route.
static bool za_ui_button_input_allowed(Cpu *c,uint64_t button){
    if(!za_ui_span(button,0xf8)||(LD64(button)!=0x7003bcfd88ull&&LD64(button)!=0x7003bd05c0ull)||
       (LD64(button+0xf0)&15)!=15)return false;
    uint64_t route=LD64(button+0xe8);
    if(route){
        if(!za_ui_span(route,8))return false;
        uint64_t vt=LD64(route);if(vt<0x7003000000ull||vt>=0x7004000000ull)return false;
        uint64_t fn=LD64(vt+0x80);if(fn<0x7000000000ull||fn>=0x7006200000ull)return false;
    }
    Cpu saved=*c;c->x[0]=button;guest_call(c,0x7000f70300ull);
    bool allowed=(c->x[0]&1)!=0;*c=saved;return allowed;
}
static unsigned za_ui_grid_cells(Cpu *c,uint64_t owner,unsigned cols,unsigned rows,ZaUiCell *cells,unsigned capacity) {
    unsigned count=cols*rows;
    if(!cols||!rows||cols>20||rows>20||count>capacity||count>400||!za_ui_span(owner,0x370))return 0;
    uint64_t selector=LD64(owner+0x368);
    if(!za_ui_span(selector,0x48)||LD64(selector)!=0x7003bd3b30ull||LD32(selector+12)!=cols||LD32(selector+16)!=rows)return 0;
    uint64_t table=LD64(selector+0x40);
    if(!za_ui_span(table,0x20))return 0;
    uint64_t entries=LD64(table+0x18);
    if(!za_ui_span(entries,count*40))return 0;
    uint64_t view=LD64(owner+0x88),child=LD64(owner+0x98);
    bool used[400]={0};unsigned found=0;
    for(unsigned steps=0;child&&steps<512;steps++,child=LD64(child+0x90)) {
        if(!za_ui_span(child,0x100)||LD64(child+0x88)!=owner)return 0;
        if(LD64(child)!=0x7003bcfd88ull)continue;
        char name[128],path[192];ZaUiRect rect;
        if(!za_ui_key_text(child+0x40,name,sizeof name)||!za_ui_key_text(child+0xa8,path,sizeof path)||
            !za_ui_rect(za_ui_pane(view,path),&rect))continue;
        if(rect.x>=1920||rect.y>=1080||rect.x+rect.width<=0||rect.y+rect.height<=0)continue;
        // The selector owns the button-to-cell binding table. It also handles
        // named controls and cells that span more than one grid position.
        uint64_t hash=LD64(child+0x40);unsigned index=count;
        for(unsigned i=0;i<count;i++)if(LD64(entries+i*40)==hash){index=i;break;}
        if(index==count)continue;
        if(used[index])return 0;
        uint64_t text=LD64(entries+index*40+8);
        // The matched name is already validated on the live child; the
        // selector may retain its own name text in the game's read-only data.
        // Native directional navigation uses bit 0 for selectability. Bit 1
        // is an independent button-state flag, not a second enable bit.
        bool enabled=text&&(LD8(entries+index*40+0x20)&1);
        bool permitted=(LD64(child+0xf0)&15)==15;
        if(permitted&&(za_menu_state()==ZA_SHOP_STATE||za_menu_state()==ZA_OUTFITS_STATE))permitted=za_ui_button_input_allowed(c,child);
        used[index]=true;cells[found++]=(ZaUiCell){rect,index,enabled&&permitted};
    }
    if(child)return 0;
    return found;
}
#endif
