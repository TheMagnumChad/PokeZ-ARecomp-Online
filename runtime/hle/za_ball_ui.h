#ifndef ZA_BALL_UI_H
#define ZA_BALL_UI_H

// Suppress the native ball list / ball reticle's outgoing visuals and the
// redundant mouse-only Move Editor tab hints through one reversible cache.
// Restore the native visibility before every layout update so its animation,
// state transitions and controller presentation continue normally.
typedef struct{
    uint64_t layout,resource,buckets,pane;
    char name[64];
    bool used,hidden,visible;
} ZaBallUi;
static ZaBallUi za_ball_uis[8];
static pthread_mutex_t za_ball_ui_lock=PTHREAD_MUTEX_INITIALIZER;
static bool za_ball_ui_identity(ZaBallUi *s,uint64_t layout){
    return s->used&&s->layout==layout&&za_ui_span(layout,0x660)&&LD64(layout)==0x7003bd5f20ull&&
        s->resource==LD64(layout+0x658)&&s->buckets==LD64(layout+0x500)&&
        s->pane==za_ui_layout_pane(layout,s->name)&&za_ui_span(s->pane,0x60);
}
static void za_ball_ui_restore_locked(ZaBallUi *s,uint64_t layout){
    if(s->hidden&&za_ball_ui_identity(s,layout))ST8(s->pane+0x58,(LD8(s->pane+0x58)&~1u)|(s->visible?1u:0u));
    s->hidden=false;
}
static void za_ball_ui_prepare(uint64_t layout){
    pthread_mutex_lock(&za_ball_ui_lock);
    for(unsigned i=0;i<8;i++)if(za_ball_uis[i].used&&za_ball_uis[i].layout==layout)za_ball_ui_restore_locked(&za_ball_uis[i],layout);
    pthread_mutex_unlock(&za_ball_ui_lock);
}
static void za_ball_ui_hide(uint64_t layout,const char *name){
    if(strlen(name)>=64)return;
    uint64_t pane=za_ui_layout_pane(layout,name);
    if(!za_ui_span(pane,0x60)||LD64(pane)<0x7003000000ull||LD64(pane)>=0x7004000000ull)return;
    pthread_mutex_lock(&za_ball_ui_lock);
    ZaBallUi *s=NULL;
    for(unsigned i=0;i<8;i++)if(za_ball_uis[i].used&&za_ball_uis[i].layout==layout&&!strcmp(za_ball_uis[i].name,name)){s=&za_ball_uis[i];break;}
    if(!s)for(unsigned i=0;i<8;i++)if(!za_ball_uis[i].used){s=&za_ball_uis[i];break;}
    if(s){
        // A repeated callback without a native update cannot overwrite the
        // original visibility with our own hidden value.
        if(!za_ball_ui_identity(s,layout)){*s=(ZaBallUi){.layout=layout,.resource=LD64(layout+0x658),.buckets=LD64(layout+0x500),.pane=pane,.used=true};snprintf(s->name,sizeof s->name,"%s",name);}
        if(!s->hidden){s->visible=(LD8(pane+0x58)&1)!=0;s->hidden=true;ST8(pane+0x58,LD8(pane+0x58)&~1u);}
    }
    pthread_mutex_unlock(&za_ball_ui_lock);
}
static void za_ball_ui_tick(uint64_t layout,bool closing){
    if(!closing)return;
    bool list=za_ui_layout_pane(layout,"N_catch")&&za_ui_layout_pane(layout,"P_ball_08");
    bool reticle=za_ui_layout_pane(layout,"N_ptn_ball_00")&&za_ui_layout_pane(layout,"N_ptn_ball_01");
    if(list||reticle)za_ball_ui_hide(layout,"all");
}
static void za_ball_ui_forget(uint64_t layout){
    pthread_mutex_lock(&za_ball_ui_lock);
    for(unsigned i=0;i<8;i++)if(za_ball_uis[i].used&&za_ball_uis[i].layout==layout){za_ball_ui_restore_locked(&za_ball_uis[i],layout);memset(&za_ball_uis[i],0,sizeof za_ball_uis[i]);}
    pthread_mutex_unlock(&za_ball_ui_lock);
}
static void za_ball_ui_reset(void){
    // Reset discards the old guest address space; never dereference it here.
    pthread_mutex_lock(&za_ball_ui_lock);memset(za_ball_uis,0,sizeof za_ball_uis);pthread_mutex_unlock(&za_ball_ui_lock);
}
#endif
