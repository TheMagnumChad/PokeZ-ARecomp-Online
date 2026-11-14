#ifndef ZA_MOUSE_UI_H
#define ZA_MOUSE_UI_H

// Extra game menus use the same grid/button framework as the supported
// screens. Host input sees rectangles and opaque identity tokens only. The
// game thread revalidates its live presenter before touching a selection.
#define ZA_UI_GROUPS 64
#define ZA_UI_CELLS 400
typedef struct {
    uint64_t owner, view, key, scene, epoch, frame;
    unsigned cols, rows, count, top, items;
    bool list;
    ZaUiCell cells[ZA_UI_CELLS];
} ZaMouseGroup;
typedef struct {
    uint64_t owner, view, key, scene, epoch, frame, serial;
    unsigned index, top, items;
    bool click, valid;
} ZaMouseUiTarget;
static ZaMouseGroup za_mouse_groups[ZA_UI_GROUPS];
static ZaMouseUiTarget za_mouse_ui_request;
static pthread_mutex_t za_mouse_ui_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t za_mouse_ui_scene,za_mouse_ui_seen,za_mouse_ui_epoch;
static _Atomic uint64_t za_mouse_ui_confirm,za_mouse_ui_buttons;
static _Atomic uint64_t za_mouse_ui_serial;
static _Atomic bool za_mouse_ui_pending;
static ZaMouseUiTarget za_mouse_ui_down,za_mouse_ui_hover;
static ZaMouseUiTarget za_mouse_ui_pointer;

static bool za_mouse_ui_scene_allowed(uint64_t scene) {
    return scene && scene!=ZA_FIELD_STATE && scene!=ZA_TITLE_STATE && scene!=0x29570d7577520f57ull &&
        scene!=ZA_BOX_STATE && scene!=ZA_BAG_STATE &&
        scene!=ZA_DEX_STATE && scene!=ZA_RESEARCH_STATE && scene!=ZA_QUEST_STATE;
}
static bool za_mouse_ui_group_allowed(uint64_t scene,unsigned cols,unsigned rows){
    // The main menu also binds read-only move / research previews through
    // GridPanel. They have button-shaped panes but no navigation controller.
    return scene!=ZA_MENU_STATE||(cols==1&&rows==6)||(cols==6&&rows==1);
}
static bool za_mouse_ui_recent(uint64_t scene) {
    uint64_t frame=renderer_present_count(),seen=atomic_load(&za_mouse_ui_seen);
    return scene && atomic_load(&za_mouse_ui_scene)==scene && seen && frame>=seen && frame-seen<=8 &&
        atomic_load(&za_mouse_ui_epoch)==atomic_load(&mouse_list_scene_epoch);
}
static bool za_mouse_ui_same(ZaMouseUiTarget a,ZaMouseUiTarget b) {
    return a.valid&&b.valid&&a.owner==b.owner&&a.view==b.view&&a.key==b.key&&a.scene==b.scene&&a.epoch==b.epoch&&a.index==b.index&&a.top==b.top&&a.items==b.items;
}
static ZaMouseUiTarget za_mouse_ui_hit(uint64_t scene,float x,float y,int w,int h) {
    ZaMouseUiTarget hit={0};
    float area=INFINITY;bool ambiguous=false;
    if(w<=0||h<=0||!isfinite(x)||!isfinite(y))return hit;
    float scale=fminf(w/1920.0f,h/1080.0f);
    x=(x-(w-1920*scale)*0.5f)/scale;y=(y-(h-1080*scale)*0.5f)/scale;
    uint64_t frame=renderer_present_count(),epoch=atomic_load(&mouse_list_scene_epoch);
    pthread_mutex_lock(&za_mouse_ui_lock);
    for(unsigned i=0;i<ZA_UI_GROUPS;i++){
        ZaMouseGroup *g=&za_mouse_groups[i];
        if(g->scene!=scene||g->epoch!=epoch||!g->frame||frame<g->frame||frame-g->frame>8||!za_mouse_ui_group_allowed(scene,g->cols,g->rows))continue;
        for(unsigned j=0;j<g->count;j++){
            ZaUiCell *cell=&g->cells[j];ZaUiRect r=cell->rect;
            if(!cell->enabled||x<r.x||x>=r.x+r.width||y<r.y||y>=r.y+r.height)continue;
            float candidate=cell->rect.width*cell->rect.height;
            // Layouts also bind broad background/preview controls. A precise
            // button inside one takes precedence; equal overlapping targets
            // are ambiguous and cannot redirect a click.
            if(candidate>area+0.01f)continue;
            if(fabsf(candidate-area)<=0.01f){ambiguous=true;continue;}
            area=candidate;ambiguous=false;
            hit=(ZaMouseUiTarget){.owner=g->owner,.view=g->view,.key=g->key,.scene=g->scene,.epoch=g->epoch,.frame=frame,.index=cell->index,.top=g->top,.items=g->items,.valid=true};
        }
    }
    if(ambiguous)hit.valid=false;
    za_mouse_ui_pointer=hit;
    pthread_mutex_unlock(&za_mouse_ui_lock);return hit;
}
static bool za_mouse_ui_queue(ZaMouseUiTarget target,bool click) {
    if(!target.valid)return false;
    if(target.scene==ZA_MOVES_STATE){
        bool current_moves=false;
        pthread_mutex_lock(&za_mouse_ui_lock);
        for(unsigned i=0;i<ZA_UI_GROUPS;i++)if(za_mouse_groups[i].owner==target.owner&&za_mouse_groups[i].scene==target.scene&&
            za_mouse_groups[i].epoch==target.epoch&&za_mouse_groups[i].cols==2&&za_mouse_groups[i].rows==3)current_moves=true;
        pthread_mutex_unlock(&za_mouse_ui_lock);
        if(current_moves)return mouse_moves_grid_queue(target.owner,target.view,target.key,target.epoch,target.index,click);
    }
    if(!click&&atomic_load(&za_mouse_ui_pending))return false;
    target.frame=renderer_present_count();target.click=click;
    pthread_mutex_lock(&za_mouse_ui_lock);
    target.serial=atomic_fetch_add(&za_mouse_ui_serial,1)+1;
    if(click){atomic_store(&za_mouse_ui_pending,true);atomic_store(&za_mouse_ui_confirm,0);atomic_store(&za_mouse_ui_buttons,0);}
    za_mouse_ui_request=target;pthread_mutex_unlock(&za_mouse_ui_lock);
    return true;
}
static void za_mouse_ui_cancel(void) {
    pthread_mutex_lock(&za_mouse_ui_lock);atomic_fetch_add(&za_mouse_ui_serial,1);memset(&za_mouse_ui_request,0,sizeof za_mouse_ui_request);
    atomic_store(&za_mouse_ui_pending,false);atomic_store(&za_mouse_ui_confirm,0);atomic_store(&za_mouse_ui_buttons,0);
    pthread_mutex_unlock(&za_mouse_ui_lock);
    za_mouse_ui_down.valid=za_mouse_ui_hover.valid=false;
}
static void za_mouse_ui_frame(uint64_t frame,uint64_t scene) {
    if(!za_mouse_ui_scene_allowed(scene)||menu_input_captured()||lumiose_scroll_menu_open()||
        atomic_load(&mouse_menu_modal_scene)==scene||
        (atomic_load(&za_mouse_ui_scene)&&atomic_load(&za_mouse_ui_scene)!=scene)){
        za_mouse_ui_cancel();return;
    }
    // Confirmation is released before another click can be accepted. Selection
    // is applied by the live presenter before its normal input handler sees A.
    uint64_t confirm=atomic_load(&za_mouse_ui_confirm);
    uint64_t buttons=0;
    if(confirm && frame>=confirm && frame-confirm<4)buttons=1ull<<BTN_A;
    else if(confirm && frame>=confirm+4){
        atomic_store(&za_mouse_ui_confirm,0);atomic_store(&za_mouse_ui_pending,false);
    }
    atomic_store(&za_mouse_ui_buttons,buttons);
    pthread_mutex_lock(&za_mouse_ui_lock);
    if(za_mouse_ui_request.valid&&(za_mouse_ui_request.scene!=scene||frame<za_mouse_ui_request.frame||frame-za_mouse_ui_request.frame>15)){
        za_mouse_ui_request.valid=false;atomic_store(&za_mouse_ui_pending,false);
    }
    pthread_mutex_unlock(&za_mouse_ui_lock);
}
static bool za_mouse_ui_focus_main(Cpu *c,uint64_t scene,uint64_t owner,unsigned cols,unsigned rows){
    if(scene!=ZA_MENU_STATE)return true;
    bool party=cols==6&&rows==1;
    if(!party&&(cols!=1||rows!=6))return true;
    uint64_t frame=renderer_present_count(),epoch=atomic_load(&mouse_list_scene_epoch),from=0;
    bool ambiguous=false;
    pthread_mutex_lock(&za_mouse_ui_lock);
    for(unsigned i=0;i<ZA_UI_GROUPS;i++){
        ZaMouseGroup *g=&za_mouse_groups[i];
        if(g->owner==owner||g->scene!=scene||g->epoch!=epoch||!g->frame||frame<g->frame||frame-g->frame>8||
            g->cols!=(party?1u:6u)||g->rows!=(party?6u:1u))continue;
        bool enabled=false;for(unsigned j=0;j<g->count;j++)if(g->cells[j].enabled)enabled=true;
        if(!enabled)continue;
        if(from&&from!=g->owner){ambiguous=true;break;}from=g->owner;
    }
    pthread_mutex_unlock(&za_mouse_ui_lock);
    if(ambiguous||!za_ui_span(from,0x550)||LD64(from)!=0x7003bd0e60ull){
        if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-main-focus] party=%d no-peer=%d ambiguous=%d\n",party,!from,ambiguous);
        return false;
    }
    uint64_t selector=LD64(from+0x368),callback=LD64(from+0x260);
    uint32_t type=LD32(from+0x258);
    if(!za_ui_span(selector,0x20)||LD64(selector)!=0x7003bd3b30ull||
        callback<0x7000000000ull||callback>=0x7006200000ull||(type!=0&&type!=1&&type!=UINT32_MAX)){
        if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-main-focus] party=%d invalid-callback=%llx type=%x from=%llx\n",party,(unsigned long long)callback,type,(unsigned long long)from);
        return false;
    }
    uint32_t oldx=LD32(from+0x304),oldy=LD32(from+0x308),index=LD32(selector+8);
    // Activate the native destination group before its selection callback.
    // Merely changing the party's index leaves the previous menu preview up.
    ST32(from+0x304,party?1:UINT32_MAX);ST32(from+0x308,party?index:0);
    Cpu saved=*c;c->x[0]=from;c->x[1]=index;
    guest_call(c,0x7000faec24ull);bool applied=(c->x[0]&1)!=0;*c=saved;
    ST32(from+0x304,oldx);ST32(from+0x308,oldy);
    if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-main-focus] party=%d from=%llx owner=%llx applied=%d\n",party,(unsigned long long)from,(unsigned long long)owner,applied);
    return applied;
}
static void za_mouse_ui_grid_tick(Cpu *c,uint64_t scene,uint64_t owner) {
    if(!za_mouse_ui_scene_allowed(scene)||!za_ui_span(owner,0x370)||LD64(owner)!=0x7003bd0e60ull)return;
    uint64_t sel=LD64(owner+0x368),view=LD64(owner+0x88);
    if(!za_ui_span(sel,0x20)||LD64(sel)!=0x7003bd3b30ull)return;
    unsigned cols=LD32(sel+12),rows=LD32(sel+16);
    bool moves=scene==ZA_MOVES_STATE&&cols==2&&rows==3&&za_ui_pane(view,"L_skill_list_00");
    // The game disables GridPanel navigation when its move-selection mode
    // owns the current moves. The live buttons remain selectable and visible.
    if(!LD8(owner+0xa2)&&!moves)return;
    if(!za_mouse_ui_group_allowed(scene,cols,rows))return;
    // Template/preview grids have no selection or activation binding. Their
    // panes can retain rectangles while another screen is focused.
    if((scene==ZA_SHOP_STATE||scene==ZA_OUTFITS_STATE)&&!LD64(owner+0x160)&&!LD64(owner+0x1a0))return;
    ZaUiCell cells[ZA_UI_CELLS];unsigned n=za_ui_grid_cells(c,owner,cols,rows,cells,ZA_UI_CELLS);
    uint64_t frame=renderer_present_count(),epoch=atomic_load(&mouse_list_scene_epoch),key=LD64(owner+0x40);
    ZaMouseUiTarget request={0};
    pthread_mutex_lock(&za_mouse_ui_lock);
    unsigned slot=0;
    for(unsigned i=0;i<ZA_UI_GROUPS;i++){
        if(za_mouse_groups[i].owner==owner&&za_mouse_groups[i].scene==scene){slot=i;break;}
        if(za_mouse_groups[i].frame<za_mouse_groups[slot].frame)slot=i;
    }
    ZaMouseGroup *g=&za_mouse_groups[slot];
    g->owner=owner;g->view=view;g->key=key;g->scene=scene;g->epoch=epoch;g->frame=frame;g->cols=cols;g->rows=rows;g->count=n;
    g->list=false;g->top=0;g->items=cols*rows;
    if(n)memcpy(g->cells,cells,n*sizeof *cells);
    if(za_mouse_ui_request.valid&&za_mouse_ui_request.owner==owner){
        // A new click first releases any earlier confirm for one full frame.
        if((!za_mouse_ui_request.click||frame>za_mouse_ui_request.frame)){request=za_mouse_ui_request;za_mouse_ui_request.valid=false;}
    }
    pthread_mutex_unlock(&za_mouse_ui_lock);
    // Hidden template groups can retain visible pane transforms. Only live,
    // enabled bindings establish that a screen is accepting mouse input.
    bool interactive=false;for(unsigned i=0;i<n;i++)if(cells[i].enabled)interactive=true;
    if(interactive){atomic_store(&za_mouse_ui_scene,scene);atomic_store(&za_mouse_ui_epoch,epoch);atomic_store(&za_mouse_ui_seen,frame);}
    if(moves)return; // The native page controller owns focus and activation.
    if(!request.valid)return;
    bool valid=request.serial==atomic_load(&za_mouse_ui_serial)&&request.scene==scene&&request.epoch==epoch&&request.view==view&&request.key==key&&request.items==cols*rows&&
        frame>=request.frame&&frame-request.frame<=15&&!menu_input_captured()&&!lumiose_scroll_menu_open()&&
        atomic_load(&mouse_menu_modal_scene)!=scene;
    bool enabled=false;for(unsigned i=0;i<n;i++)if(cells[i].index==request.index&&cells[i].enabled)enabled=true;
    if(!valid||!enabled){if(request.click&&request.serial==atomic_load(&za_mouse_ui_serial))atomic_store(&za_mouse_ui_pending,false);return;}
    if(!za_mouse_ui_focus_main(c,scene,owner,cols,rows)){
        if(request.click&&request.serial==atomic_load(&za_mouse_ui_serial))atomic_store(&za_mouse_ui_pending,false);
        return;
    }
    unsigned index=request.index,orientation=LD32(sel+0x1c);
    unsigned x=orientation?index/rows:index%cols,y=orientation?index%rows:index/cols;
    uint32_t old=LD32(sel+8),oldx=LD32(owner+0x304),oldy=LD32(owner+0x308);
    ST32(owner+0x304,x);ST32(owner+0x308,y);
    Cpu saved=*c;c->sp-=32;ST32(c->sp,x);ST32(c->sp+4,y);c->x[0]=owner;c->x[1]=c->sp;c->x[2]=1;c->x[3]=0;
    guest_call(c,0x700098d4e0ull);*c=saved;
    if(LD32(sel+8)!=index){ST32(owner+0x304,oldx);ST32(owner+0x308,oldy);if(request.click&&request.serial==atomic_load(&za_mouse_ui_serial))atomic_store(&za_mouse_ui_pending,false);return;}
    if(old!=index){c->x[0]=owner;c->x[1]=index;c->x[2]=old;c->x[3]=0;c->x[4]=1;guest_call(c,0x7000fad610ull);*c=saved;}
    if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-ui] frame=%llu scene=%llx owner=%llx index=%u click=%d\n",(unsigned long long)frame,(unsigned long long)scene,(unsigned long long)owner,index,request.click);
    if(request.click){
        pthread_mutex_lock(&za_mouse_ui_lock);
        if(request.serial==atomic_load(&za_mouse_ui_serial)){
            bool ready=atomic_load(&za_mouse_ui_pending)&&za_menu_state()==scene;
            // The script-facing button API only animates some presenters.
            // Their screen controller owns the action. Confirm the validated
            // selection through that controller on the following frame.
            bool applied=ready;
            if(applied)atomic_store(&za_mouse_ui_confirm,frame+1);
            if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-ui-decide] owner=%llx index=%u applied=%d\n",(unsigned long long)owner,index,applied);
            if(!applied)atomic_store(&za_mouse_ui_pending,false);
        }
        pthread_mutex_unlock(&za_mouse_ui_lock);
    }
}
// Shop and outfit lists bind the same native button children as the Satchel.
// Read the current bindings and panes, including the separate colour picker.
// A list owns its target identity, so another visible list cannot consume it.
static bool za_mouse_ui_shop_list(Cpu *c,uint64_t scene,uint64_t owner,unsigned cols,unsigned rows,unsigned top,unsigned *observed_count){
    unsigned count=*observed_count;
    if((scene!=ZA_SHOP_STATE&&scene!=ZA_OUTFITS_STATE)||cols!=1||!rows||rows>20||!count||count>1024||
        !za_ui_span(owner,0x690)||LD64(owner)!=0x7003bd1e00ull||!LD8(owner+0xa2))return false;
    uint64_t frame=renderer_present_count(),epoch=atomic_load(&mouse_list_scene_epoch),view=LD64(owner+0x88),key=LD64(owner+0x40);
    // A virtual stock list may keep selector capacity 1024. Its live data
    // source, not that capacity, determines which rows exist.
    uint64_t source=LD64(owner+0x148);
    if(!za_ui_span(source,8))return false;
    uint64_t source_vt=LD64(source);if(source_vt<0x7003000000ull||source_vt>=0x7004000000ull)return false;
    uint64_t count_fn=LD64(source_vt+0x80);if(count_fn<0x7000000000ull||count_fn>=0x7006200000ull)return false;
    Cpu saved_count=*c;c->x[0]=source;guest_call(c,count_fn);unsigned actual_count=(uint32_t)c->x[0];*c=saved_count;
    if(!actual_count||actual_count>=1024)return false;
    if(count>actual_count)count=actual_count;
    *observed_count=count;
    // Native ListPanel's binding pool maps each recycled button key to its
    // current absolute item. Copy suffixes identify widgets, not list rows.
    // This is the same table searched by the game's GetBinding (fe8340).
    uint64_t pool=LD64(owner+0x5c0);
    if(!za_ui_span(pool,16))return false;
    uint64_t begin=LD64(pool),end=LD64(pool+8);
    if(end<begin||(end-begin)%48||(end-begin)/48>128||!za_ui_span(begin,end-begin))return false;
    ZaUiCell cells[20];unsigned n=0;bool used[20]={0};
    uint64_t child=LD64(owner+0x98);
    for(unsigned step=0;child&&step<128;step++,child=LD64(child+0x90)){
        if(!za_ui_span(child,0x100)||LD64(child+0x88)!=owner)return false;
        if(LD64(child)!=0x7003bcfd88ull&&LD64(child)!=0x7003bd05c0ull)continue;
        char name[128],path[192];ZaUiRect rect;
        if(!za_ui_key_text(child+0x40,name,sizeof name)||!za_ui_key_text(child+0xa8,path,sizeof path)||
            !za_ui_rect(za_ui_pane(view,path),&rect))continue;
        unsigned absolute=UINT32_MAX;
        for(uint64_t entry=begin;entry<end;entry+=48){
            if(LD8(entry+0x20)||LD64(entry)!=LD64(child+0x40)||LD64(entry+16)!=strlen(name))continue;
            if(absolute!=UINT32_MAX)return false;
            absolute=LD32(entry+0x24);
        }
        if(absolute<top||absolute>=count||absolute-top>=rows)continue;
        unsigned row=absolute-top;
        if(row>=rows||rect.x>=1920||rect.y>=1080||rect.x+rect.width<=0||rect.y+rect.height<=0)continue;
        if(used[row])return false;
        uint64_t selection=LD64(owner+0x630);
        bool selected=za_ui_span(selection,0x28)&&LD32(selection+16)==top&&LD32(selection+20)==row;
        used[row]=true;cells[n++]=(ZaUiCell){rect,row,top+row<count&&(selected||za_ui_button_input_allowed(c,child))};
    }
    if(child||!n)return false;
    ZaMouseUiTarget request={0},pointer={0};
    pthread_mutex_lock(&za_mouse_ui_lock);
    unsigned slot=0;
    for(unsigned i=0;i<ZA_UI_GROUPS;i++){
        if(za_mouse_groups[i].owner==owner&&za_mouse_groups[i].scene==scene){slot=i;break;}
        if(za_mouse_groups[i].frame<za_mouse_groups[slot].frame)slot=i;
    }
    ZaMouseGroup *g=&za_mouse_groups[slot];
    *g=(ZaMouseGroup){.owner=owner,.view=view,.key=key,.scene=scene,.epoch=epoch,.frame=frame,.cols=cols,.rows=rows,.count=n,.top=top,.items=count,.list=true};
    memcpy(g->cells,cells,n*sizeof *cells);
    pointer=za_mouse_ui_pointer;
    if(za_mouse_ui_request.valid&&za_mouse_ui_request.owner==owner&&
        (!za_mouse_ui_request.click||frame>za_mouse_ui_request.frame)){
        request=za_mouse_ui_request;za_mouse_ui_request.valid=false;
    }
    pthread_mutex_unlock(&za_mouse_ui_lock);
    atomic_store(&za_mouse_ui_scene,scene);atomic_store(&za_mouse_ui_epoch,epoch);atomic_store(&za_mouse_ui_seen,frame);
    bool active=pointer.valid&&pointer.owner==owner&&pointer.view==view&&pointer.key==key&&pointer.scene==scene&&pointer.epoch==epoch&&pointer.items==count;
    if(!request.valid)return active;
    bool valid=request.serial==atomic_load(&za_mouse_ui_serial)&&request.scene==scene&&request.epoch==epoch&&
        request.view==view&&request.key==key&&request.top==top&&request.items==count&&frame>=request.frame&&frame-request.frame<=15&&
        !menu_input_captured()&&!lumiose_scroll_menu_open()&&atomic_load(&mouse_menu_modal_scene)!=scene;
    bool enabled=false;for(unsigned i=0;i<n;i++)if(cells[i].index==request.index&&cells[i].enabled)enabled=true;
    // Selecting a shop row can disable that button's navigation route while
    // the screen still accepts A for the selected item. Mouse-up confirms the
    // already selected row; it must not require another navigation action.
    uint64_t selected=LD64(owner+0x630);
    bool selected_click=request.click&&za_ui_span(selected,0x28)&&LD64(selected)==0x7003bd3ce0ull&&
        LD32(selected+16)==top&&LD32(selected+20)==request.index;
    if(valid&&(enabled||selected_click)){
        uint64_t source=LD64(owner+0x148);
        valid=za_ui_span(source,8);uint64_t vt=valid?LD64(source):0;
        valid=valid&&vt>=0x7003000000ull&&vt<0x7004000000ull;
        uint64_t fn=valid?LD64(vt+0x80):0;valid=valid&&fn>=0x7000000000ull&&fn<0x7006200000ull;
        if(valid){
            Cpu saved=*c;c->x[0]=source;guest_call(c,fn);unsigned actual=(uint32_t)c->x[0];*c=saved;
            valid=top+request.index<actual;
            if(valid){
                // A new mouse target supersedes the selection pinned by wheel
                // scrolling before the native setter invokes its callbacks.
                bool guarded=atomic_load(&za_list_guard)&&atomic_load(&za_list_guard_owner)==owner;
                int previous=atomic_load(&za_list_guard_selected);
                if(guarded)atomic_store(&za_list_guard_selected,(int)(top+request.index));
                c->x[0]=owner;c->x[1]=request.index;c->x[2]=top;guest_call(c,0x7000fc2630ull);*c=saved;
                uint64_t selector=LD64(owner+0x630);
                valid=za_ui_span(selector,0x28)&&LD32(selector+16)==top&&LD32(selector+20)==request.index;
                if(!valid&&guarded)atomic_store(&za_list_guard_selected,previous);
                if(valid&&za_list.owner==owner&&za_list.pinned){
                    za_list.selected=(int)(top+request.index);za_list.recover=false;
                    atomic_store(&za_list_guard_selected,za_list.selected);
                }
            }
        }
    }else valid=false;
    if(request.click&&request.serial==atomic_load(&za_mouse_ui_serial)){
        if(valid&&za_menu_state()==scene)atomic_store(&za_mouse_ui_confirm,frame+1);
        else atomic_store(&za_mouse_ui_pending,false);
    }
    if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-shop-list] frame=%llu owner=%llx top=%u row=%u click=%d applied=%d\n",
        (unsigned long long)frame,(unsigned long long)owner,top,request.index,request.click,valid);
    return active||valid;
}
static void za_mouse_ui_reset(void) {
    za_mouse_ui_cancel();
    pthread_mutex_lock(&za_mouse_ui_lock);memset(za_mouse_groups,0,sizeof za_mouse_groups);memset(&za_mouse_ui_pointer,0,sizeof za_mouse_ui_pointer);pthread_mutex_unlock(&za_mouse_ui_lock);
    atomic_store(&za_mouse_ui_scene,0);atomic_store(&za_mouse_ui_seen,0);atomic_store(&za_mouse_ui_epoch,0);
}
#endif
