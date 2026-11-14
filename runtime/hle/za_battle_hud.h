#ifndef ZA_BATTLE_HUD_H
#define ZA_BATTLE_HUD_H

// Native 2.0.2 move parts retain their own content, cooldowns and animations.
// Only their parents' transforms change; no move identity or input is changed.
typedef struct {
    uint64_t layout, resource, buckets, panes[9];
    uint32_t original[9][4];
    bool occupied, horizontal;
} ZaBattleHud;
static ZaBattleHud za_battle_huds[4];
static pthread_mutex_t za_battle_hud_lock=PTHREAD_MUTEX_INITIALIZER;
static const unsigned za_hud_transform_offsets[4]={0x30,0x34,0x48,0x4c};
static bool za_hud_parent_point(uint64_t pane,float *x,float *y){
    for(unsigned depth=0;pane&&depth<32;depth++){
        if(!za_ui_span(pane,0x60))return false;
        float tx=za_ui_float(pane+0x30),ty=za_ui_float(pane+0x34);
        float sx=za_ui_float(pane+0x48),sy=za_ui_float(pane+0x4c),angle=za_ui_float(pane+0x44);
        if(!isfinite(tx)||!isfinite(ty)||!isfinite(sx)||!isfinite(sy)||!isfinite(angle)||
           fabsf(tx)>10000||fabsf(ty)>10000||fabsf(sx)>100||fabsf(sy)>100)return false;
        float co=cosf(angle*0.01745329252f),si=sinf(angle*0.01745329252f);
        float px=*x*sx,py=*y*sy;*x=tx+px*co-py*si;*y=ty+px*si+py*co;
        unsigned anchors=LD8(pane+0x5b),ax=(anchors>>4)&3,ay=(anchors>>6)&3;
        pane=LD64(pane+0x18);if(ax==3||ay==3)return false;
        if(pane&&(ax||ay)){
            if(!za_ui_span(pane,0x60))return false;
            float w=za_ui_float(pane+0x50),h=za_ui_float(pane+0x54);
            if(!isfinite(w)||!isfinite(h))return false;
            *x+=ax==1?-w/2:ax==2?w/2:0;*y+=ay==1?h/2:ay==2?-h/2:0;
        }
    }
    return !pane&&isfinite(*x)&&isfinite(*y);
}
static bool za_hud_center(uint64_t pane,float *x,float *y){
    if(!za_ui_span(pane,0x60)||(LD8(pane+0x5b)&0xf0))return false;
    uint64_t parent=LD64(pane+0x18);
    float ox=0,oy=0,xx=1,xy=0,yx=0,yy=1;
    if(!za_hud_parent_point(parent,&ox,&oy)||!za_hud_parent_point(parent,&xx,&xy)||
       !za_hud_parent_point(parent,&yx,&yy))return false;
    xx-=ox;xy-=oy;yx-=ox;yy-=oy;float det=xx*yy-yx*xy;
    if(!isfinite(det)||fabsf(det)<0.0001f)return false;
    // The game can recenter a layout through an animated parent. Position in
    // screen space, then invert that parent's transform instead of assuming 0.
    // Leave room for the party bar on the right, moving the move cards and
    // their attached labels/keycaps together.
    float dx=-120-ox,dy=-385-oy;*x=(dx*yy-dy*yx)/det;*y=(dy*xx-dx*xy)/det;
    return isfinite(*x)&&isfinite(*y)&&fabsf(*x)<10000&&fabsf(*y)<10000;
}
static void za_hud_float(uint64_t pane,unsigned offset,float value){
    uint32_t bits;memcpy(&bits,&value,4);ST32(pane+offset,bits);
}
static bool za_battle_hud_native_positions(const uint64_t panes[9]){
    const float original[4][2]={{0,46},{54,-9},{-54,-9},{0,-62}};
    unsigned seen=0;
    for(unsigned i=0;i<4;i++){
        float x=za_ui_float(panes[1+i]+0x30),y=za_ui_float(panes[1+i]+0x34);
        unsigned slot=4;for(unsigned j=0;j<4;j++)if(x==original[j][0]&&y==original[j][1])slot=j;
        if(slot==4||(seen&(1u<<slot)))return false;seen|=1u<<slot;
    }
    return seen==15;
}
static void za_battle_hud_apply(ZaBattleHud *hud,bool keyboard,const float text_x[4]){
    if(!keyboard){
        if(hud->horizontal)for(unsigned i=0;i<9;i++)for(unsigned j=0;j<4;j++)
            ST32(hud->panes[i]+za_hud_transform_offsets[j],hud->original[i][j]);
        hud->horizontal=false;return;
    }
    // Native identities are X,A,Y,B. Default dedicated move keys are A=Z,
    // B=X, X=C, Y=V. Keep that logical order independently of pad bindings.
    const float x[4]={100,-300,300,-100};float center_x,center_y;
    if(!za_hud_center(hud->panes[0],&center_x,&center_y))return;
    za_hud_float(hud->panes[0],0x30,center_x);za_hud_float(hud->panes[0],0x34,center_y);
    for(unsigned i=0;i<4;i++){
        za_hud_float(hud->panes[1+i],0x30,x[i]);za_hud_float(hud->panes[1+i],0x34,0);
        za_hud_float(hud->panes[5+i],0x30,-text_x[i]*0.65f);
        za_hud_float(hud->panes[5+i],0x34,-70);
        za_hud_float(hud->panes[5+i],0x48,0.65f);za_hud_float(hud->panes[5+i],0x4c,0.65f);
    }
    hud->horizontal=true;
}
static void za_battle_hud_tick(uint64_t layout,bool keyboard){
    uint64_t panes[9],labels[4];float text_x[4];
    panes[0]=za_ui_layout_pane(layout,"N_pos_00");
    if(!za_ui_span(panes[0],0x60)||LD64(panes[0])!=0x7003c93b08ull)return;
    for(unsigned i=0;i<4;i++){
        char name[64];snprintf(name,sizeof name,"L_waza_%02u",i);panes[1+i]=za_ui_layout_pane(layout,name);
        snprintf(name,sizeof name,"L_waza_%02u/N_text_00",i);panes[5+i]=za_ui_layout_pane(layout,name);
        snprintf(name,sizeof name,"L_waza_%02u/T_00",i);labels[i]=za_ui_layout_pane(layout,name);
        if(!za_ui_span(panes[1+i],0x60)||LD64(panes[1+i])!=0x7003c93c68ull||
           !za_ui_span(panes[5+i],0x60)||LD64(panes[5+i])!=0x7003c93b08ull||
           !za_ui_span(labels[i],0x60)||LD64(labels[i])!=0x7003bd6808ull)return;
        uint64_t parent=LD64(panes[1+i]+0x18);
        if(!za_ui_span(parent,0x60)||LD64(parent+0x18)!=panes[0])return;
        text_x[i]=za_ui_float(labels[i]+0x30);if(!isfinite(text_x[i])||fabsf(text_x[i])>1000)return;
    }
    if(pthread_mutex_trylock(&za_battle_hud_lock)!=0)return;
    ZaBattleHud *hud=NULL;
    for(unsigned i=0;i<4;i++)if(za_battle_huds[i].occupied&&za_battle_huds[i].layout==layout){hud=&za_battle_huds[i];break;}
    if(!hud){for(unsigned i=0;i<4;i++)if(!za_battle_huds[i].occupied){hud=&za_battle_huds[i];break;}}
    if(!hud){pthread_mutex_unlock(&za_battle_hud_lock);return;}
    uint64_t resource=LD64(layout+0x658),buckets=LD64(layout+0x500);
    bool identity=hud->occupied&&hud->layout==layout&&hud->resource==resource&&hud->buckets==buckets&&!memcmp(hud->panes,panes,sizeof panes);
    if(!identity){
        if(!za_battle_hud_native_positions(panes)){pthread_mutex_unlock(&za_battle_hud_lock);return;}
        memset(hud,0,sizeof *hud);hud->occupied=true;hud->layout=layout;hud->resource=resource;hud->buckets=buckets;
        memcpy(hud->panes,panes,sizeof panes);
        for(unsigned i=0;i<9;i++)for(unsigned j=0;j<4;j++)hud->original[i][j]=LD32(panes[i]+za_hud_transform_offsets[j]);
    }
    bool changed=hud->horizontal!=keyboard;
    za_battle_hud_apply(hud,keyboard,text_x);
    if(changed&&getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[keyboard-attack-row] layout=%llx horizontal=%d\n",(unsigned long long)layout,keyboard);
    pthread_mutex_unlock(&za_battle_hud_lock);
}
static void za_battle_hud_reset(void){
    pthread_mutex_lock(&za_battle_hud_lock);memset(za_battle_huds,0,sizeof za_battle_huds);pthread_mutex_unlock(&za_battle_hud_lock);
}
static void za_battle_hud_forget(uint64_t layout){
    pthread_mutex_lock(&za_battle_hud_lock);
    for(unsigned i=0;i<4;i++)if(za_battle_huds[i].layout==layout)memset(&za_battle_huds[i],0,sizeof za_battle_huds[i]);
    pthread_mutex_unlock(&za_battle_hud_lock);
}
#endif
