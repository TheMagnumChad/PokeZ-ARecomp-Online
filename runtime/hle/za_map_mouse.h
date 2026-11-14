#ifndef ZA_MAP_MOUSE_H
#define ZA_MAP_MOUSE_H

// Wheel requests contain no guest pointer. The game's active map controller
// consumes them in its own update, through its native zoom event handler.
static _Atomic int za_map_wheel_steps;
static _Atomic uint64_t za_map_wheel_frame,za_map_wheel_epoch;
static _Thread_local bool za_map_busy;
static void za_map_wheel_queue(int steps){
    atomic_store(&za_map_wheel_frame,renderer_present_count());
    atomic_store(&za_map_wheel_epoch,atomic_load(&mouse_list_scene_epoch));
    int old=atomic_load(&za_map_wheel_steps),next;
    do{next=old+steps;if(next>20)next=20;if(next<-20)next=-20;}
    while(!atomic_compare_exchange_weak(&za_map_wheel_steps,&old,next));
}
static void za_map_wheel_reset(void){
    atomic_store(&za_map_wheel_steps,0);atomic_store(&za_map_wheel_frame,0);atomic_store(&za_map_wheel_epoch,0);
}
void input_native_map_tick(Cpu *c){
    if(za_map_busy||!atomic_load(&za_map_wheel_steps))return;
    uint64_t frame=renderer_present_count(),queued=atomic_load(&za_map_wheel_frame);
    if(za_menu_state()!=ZA_BIGMAP_STATE||menu_input_captured()||lumiose_scroll_menu_open()||
        atomic_load(&mouse_menu_modal_scene)==ZA_BIGMAP_STATE||frame<queued||frame-queued>15||
        atomic_load(&za_map_wheel_epoch)!=atomic_load(&mouse_list_scene_epoch)){
        za_map_wheel_reset();return;
    }
    uint64_t owner=c->x[0];
    if(!za_ui_span(owner,0x148))return;
    uint64_t vt=LD64(owner);
    if(vt!=0x7003df4ce0ull&&vt!=0x7003df4f20ull)return;
    uint64_t model=LD64(owner+0xe0),view=LD64(owner+0xe8);
    if(!za_ui_span(model,0x290)||!za_ui_span(view,0xf0))return;
    // The owning controller and native model gate zoom availability. This
    // view has no shared pane-cache wrapper; querying it as a ListView would
    // reject a valid map. The native handler also rejects its blocked modes.
    int steps=atomic_exchange(&za_map_wheel_steps,0);
    // This verified map has three native zoom levels. Excess wheel movement
    // stops at its limits instead of being replayed as held controller input.
    unsigned events=(unsigned)abs(steps);if(events>2)events=2;
    za_map_busy=true;
    for(unsigned i=0;i<events;i++){
        uint32_t before=LD32(0x7003f287c8ull),bits;float direction=steps>0?1.0f:-1.0f;
        if(before>2)break;
        memcpy(&bits,&direction,sizeof bits);Cpu saved=*c;c->sp-=32;
        ST32(c->sp,0);ST32(c->sp+4,bits);c->x[0]=owner;c->x[1]=c->sp;
        guest_call(c,0x7000a370b4ull);*c=saved;
        uint32_t after=LD32(0x7003f287c8ull);
        if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-map-zoom] frame=%llu owner=%llx before=%u after=%u direction=%d\n",
            (unsigned long long)frame,(unsigned long long)owner,before,after,steps>0?1:-1);
        if(after==before)break;
    }
    za_map_busy=false;
}
#endif
