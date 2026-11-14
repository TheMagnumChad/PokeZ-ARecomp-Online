#ifndef ZA_MOVES_MOUSE_H
#define ZA_MOVES_MOUSE_H

// Move Editor has its own page controller. Its disabled GridPanel is a view,
// not the owner of keyboard focus. Use the page's selection/decide methods
// and keep clicks ordered independently of hover and unrelated UI presenters.
#define ZA_MOVE_ACTIONS 32
typedef struct {
    uint64_t owner, view, key, epoch, frame, serial, time, prepared_frame;
    unsigned index, top;
    bool grid, prepared;
} ZaMoveAction;
static ZaMoveAction move_actions[ZA_MOVE_ACTIONS], move_hover;
static unsigned move_head, move_count;
static bool move_hover_valid;
static uint64_t move_serial, move_epoch, move_last_frame, move_back_until;
static pthread_mutex_t move_exec_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool move_exec_busy;
static bool move_back_key_down, move_back_mouse_down;

static void mouse_moves_cancel(void) {
    pthread_mutex_lock(&mouse_moves_lock);
    move_head=move_count=0;move_hover_valid=false;++move_serial;
    pthread_mutex_unlock(&mouse_moves_lock);
}
static void mouse_moves_back(void) {
    pthread_mutex_lock(&mouse_moves_lock);
    move_head=move_count=0;move_hover_valid=false;
    move_back_until=renderer_present_count()+10;
    move_epoch=atomic_load(&mouse_list_scene_epoch);++move_serial;
    pthread_mutex_unlock(&mouse_moves_lock);
    atomic_store(&mouse_menu_modal_scene,0);atomic_store(&box_context_request,0);
    atomic_store(&box_context_click_pending,false);atomic_store(&box_context_confirm_start,0);atomic_store(&box_context_buttons,0);
    box_context_hover=box_context_down=-1;
    // Keep the native cancel route. It dismisses an unfinished choice or
    // leaves the page according to its actual state, including an empty tab.
    // A single release/press edge cannot repeat into the parent menu.
    atomic_store(&menu_back_request,true);
    if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[moves-page-back] f=%llu\n",(unsigned long long)renderer_present_count());
}
static void mouse_moves_bind_publish(uint64_t owner,uint64_t view,unsigned top,unsigned count) {
    if(LD32(owner+0x228)!=0||LD64(owner+0x230)!=0x7002d3c34cull)return;
    pthread_mutex_lock(&mouse_moves_lock);
    move_binding=(ZaMoveBinding){owner,view,LD64(owner+0x40),atomic_load(&mouse_list_scene_epoch),renderer_present_count(),top,count};
    pthread_mutex_unlock(&mouse_moves_lock);
}
static bool mouse_moves_queue(uint64_t owner,uint64_t view,uint64_t key,uint64_t epoch,unsigned index,unsigned top,bool grid,bool click) {
    uint64_t frame=renderer_present_count();
    if(za_menu_state()!=ZA_MOVES_STATE||epoch!=atomic_load(&mouse_list_scene_epoch))return false;
    pthread_mutex_lock(&mouse_moves_lock);
    if(frame<move_back_until){pthread_mutex_unlock(&mouse_moves_lock);return false;}
    if(move_epoch!=epoch){move_head=move_count=0;move_hover_valid=false;move_epoch=epoch;}
    ZaMoveAction a={.owner=owner,.view=view,.key=key,.epoch=epoch,.frame=frame,.serial=++move_serial,.time=SDL_GetTicksNS(),.index=index,.top=top,.grid=grid};
    bool accepted=true;
    if(click){
        if(move_count==ZA_MOVE_ACTIONS)accepted=false;
        else{move_actions[(move_head+move_count)%ZA_MOVE_ACTIONS]=a;++move_count;move_hover_valid=false;}
    }else if(!move_count){move_hover=a;move_hover_valid=true;}
    else accepted=false;
    pthread_mutex_unlock(&mouse_moves_lock);return accepted;
}
static bool mouse_moves_grid_queue(uint64_t owner,uint64_t view,uint64_t key,uint64_t epoch,unsigned index,bool click) {
    static const int slots[6]={0,-1,2,1,3,-1};
    if(index>=6||slots[index]<0)return false;
    return mouse_moves_queue(owner,view,key,epoch,index,0,true,click);
}
static void mouse_moves_list_queue(unsigned row,bool click) {
    pthread_mutex_lock(&mouse_moves_lock);ZaMoveBinding b=move_binding;pthread_mutex_unlock(&mouse_moves_lock);
    uint64_t frame=renderer_present_count();
    if(b.frame&&frame>=b.frame&&frame-b.frame<=8&&row<6&&b.top+row<b.count)
        mouse_moves_queue(b.owner,b.view,b.key,b.epoch,row,b.top,false,click);
}
static bool mouse_moves_live_list(ZaMoveBinding b,uint64_t frame) {
    if(!b.frame||frame<b.frame||frame-b.frame>8||b.epoch!=atomic_load(&mouse_list_scene_epoch)||
       !za_ui_span(b.owner,0x938)||LD64(b.owner)!=0x7003bd1e00ull||LD64(b.owner+0x88)!=b.view||LD64(b.owner+0x40)!=b.key||
       LD32(b.owner+0x228)!=0||LD64(b.owner+0x230)!=0x7002d3c34cull)return false;
    uint64_t sel=LD64(b.owner+0x630);
    return za_ui_span(sel,0x48)&&LD64(sel)==0x7003bd3ce0ull&&LD32(sel+32)==1&&LD32(sel+36)==6;
}
static void mouse_moves_call(Cpu *c,uint64_t pc,uint64_t x0,uint64_t x1,uint64_t x2) {
    Cpu saved=*c;c->x[0]=x0;c->x[1]=x1;c->x[2]=x2;guest_call(c,pc);*c=saved;
}
void input_native_moves_flush(Cpu *c) {
    if(move_exec_busy||za_menu_state()!=ZA_MOVES_STATE||menu_input_captured()||lumiose_scroll_menu_open()||
       pthread_mutex_trylock(&move_exec_lock))return;
    move_exec_busy=true;
    uint64_t frame=renderer_present_count(),epoch=atomic_load(&mouse_list_scene_epoch),now=SDL_GetTicksNS();
    ZaMoveBinding b;ZaMoveAction a={0};bool click=false,have=false;
    pthread_mutex_lock(&mouse_moves_lock);
    if(move_epoch!=epoch){move_head=move_count=0;move_hover_valid=false;move_epoch=epoch;}
    b=move_binding;
    if(move_count){a=move_actions[move_head];have=click=true;}
    else if(move_hover_valid){a=move_hover;have=true;}
    pthread_mutex_unlock(&mouse_moves_lock);
    if(!have||move_last_frame==frame||!mouse_moves_live_list(b,frame))goto done;
    if(a.epoch!=epoch||frame<a.frame||frame-a.frame>180||now<a.time||now-a.time>(click?3000000000ull:300000000ull))goto discard;
    // The callback contains a native weak reference. Resolve and balance its
    // owned reference on every call; never keep the page alive after exit.
    Cpu saved=*c;c->sp-=32;ST64(c->sp,0);c->x[0]=b.owner+0x210;c->x[8]=c->sp;
    guest_call(c,0x70009b4dd8ull);uint64_t input=LD64(saved.sp-32);*c=saved;
    if(!input)goto done;
    uint64_t vt=LD64(input),release=vt>=0x7003000000ull&&vt<0x7004000000ull?LD64(vt+0x60):0;
    if(release<0x7000000000ull||release>=0x7006200000ull)goto done;
    bool completed=false,valid=vt==0x7003e226a8ull&&za_ui_span(input,0x140);
    uint64_t model=valid?LD64(input+0x118):0,view=valid?LD64(input+0x128):0,controller=valid?LD64(input+0x130):0,state=valid?LD64(input+0x138):0;
    valid=valid&&za_ui_span(model,0x150)&&LD64(model)==0x7003e22558ull&&za_ui_span(view,0x100)&&LD64(view)==0x7003e23900ull&&
        za_ui_span(controller,0xf8)&&LD64(controller+0xf0)==b.owner&&za_ui_span(state,0x108)&&LD64(state+0xe0)==model;
    if(!valid)goto release_ref;
    if(LD8(model+0xfc))goto release_ref; // Let native transitions finish before another action.
    if(have&&click&&!a.prepared&&getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[moves-page-before] f=%llu grid=%d index=%u mode=%u state=%llx pending=%u slot=%u candidate=%u\n",(unsigned long long)frame,a.grid,a.index,LD32(model+0x50),(unsigned long long)LD64(state),LD8(state+0xd8),LD32(model+0xa8),LD16(model+0xec));
    uint64_t selector=LD64(b.owner+0x630);
    if(a.grid){
        static const int slots[6]={0,-1,2,1,3,-1};
        valid=a.index<6&&slots[a.index]>=0&&za_ui_span(a.owner,0x550)&&LD64(a.owner)==0x7003bd0e60ull&&
            LD64(controller+0xe8)==a.owner&&LD64(a.owner+0x88)==a.view&&LD64(a.owner+0x40)==a.key;
        uint64_t begin=LD64(model+0x120);valid=valid&&za_ui_span(begin,8)&&LD64(model+0x128)==begin+8;
        if(valid&&!a.prepared){
            unsigned slot=slots[a.index];
            if(click){
                if(LD32(model+0x50)==0)mouse_moves_call(c,0x7002d3bdb0ull,input,1,0);
                mouse_moves_call(c,0x7002d3bd3cull,input,slot,0);
            }else if(LD32(model+0x50)==0)mouse_moves_call(c,0x7000a35ea0ull,view,slot,0);
            else mouse_moves_call(c,0x7002d3bd3cull,input,slot,0);
        }
    }else{
        unsigned absolute=a.top+a.index,top=LD32(selector+16);
        valid=a.owner==b.owner&&a.view==b.view&&a.key==b.key&&a.index<6&&absolute<LD32(selector+8)&&absolute>=top&&absolute-top<6;
        if(valid&&!a.prepared){
            if(click&&LD32(model+0x50)==1)mouse_moves_call(c,0x7000b56674ull,model,0,1);
            pthread_mutex_lock(&za_list_lock);
            if(za_list.owner==b.owner&&za_list.pinned){za_list.selected=absolute;za_list.recover=false;atomic_store(&za_list_guard_selected,za_list.selected);}
            pthread_mutex_unlock(&za_list_lock);
            mouse_moves_call(c,0x7000fc2630ull,b.owner,absolute-top,top);
        }
    }
    if(valid&&click){
        pthread_mutex_lock(&mouse_moves_lock);
        bool current=move_count&&move_actions[move_head].serial==a.serial;
        if(current&&!a.prepared){move_actions[move_head].prepared=true;move_actions[move_head].prepared_frame=frame;}
        pthread_mutex_unlock(&mouse_moves_lock);
        if(!current)goto release_ref;
        // Mode notifications switch native page states on the next update.
        // Confirm only after that update, never through the previous panel.
        if(!a.prepared||frame<=a.prepared_frame){move_last_frame=frame;goto release_ref;}
        unsigned mode=LD32(model+0x50);
        uint64_t expected=mode==0?0x7003e23348ull:mode==1?0x7003e231d0ull:mode==2?0x7003e22e88ull:0;
        if(!expected||LD64(state)!=expected)goto release_ref;
        if(LD8(model+0xfc))goto release_ref;
        mouse_moves_call(c,0x7002d3c354ull,b.owner+0x210,0,0);
        completed=true;
        if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[moves-page-click] f=%llu grid=%d index=%u mode=%u slot=%u\n",(unsigned long long)frame,a.grid,a.index,LD32(model+0x50),LD32(model+0xa8));
    }
    move_last_frame=frame;
release_ref:
    mouse_moves_call(c,release,input,0,0);
    if(!valid||!click||completed){
discard:
        pthread_mutex_lock(&mouse_moves_lock);
        if(click&&move_count&&move_actions[move_head].serial==a.serial){move_head=(move_head+1)%ZA_MOVE_ACTIONS;--move_count;}
        else if(!click&&move_hover_valid&&move_hover.serial==a.serial)move_hover_valid=false;
        pthread_mutex_unlock(&mouse_moves_lock);
    }
done:
    move_exec_busy=false;pthread_mutex_unlock(&move_exec_lock);
}
#endif
