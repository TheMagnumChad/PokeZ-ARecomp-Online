#ifndef ZA_SHOP_QUANTITY_H
#define ZA_SHOP_QUANTITY_H

// The number popup is separate from the shop's item list. Publish only its
// live pane bounds; no guest pointer crosses into the host event handler.
typedef struct {ZaUiRect up,down,confirm;uint64_t layout,frame,epoch;bool shown,valid;} ZaShopQuantity;
static pthread_mutex_t za_shop_quantity_lock=PTHREAD_MUTEX_INITIALIZER;
static ZaShopQuantity za_shop_quantity;
static int za_shop_quantity_actions[32];
static _Atomic int za_shop_quantity_down=-2;
static unsigned za_shop_quantity_head,za_shop_quantity_count;
static uint64_t za_shop_quantity_epoch,za_shop_quantity_until;
static _Atomic uint64_t za_shop_quantity_buttons;

static bool za_shop_quantity_active(void){
    uint64_t frame=renderer_present_count();
    pthread_mutex_lock(&za_shop_quantity_lock);ZaShopQuantity q=za_shop_quantity;pthread_mutex_unlock(&za_shop_quantity_lock);
    return za_menu_state()==ZA_SHOP_STATE&&q.shown&&q.epoch==atomic_load(&mouse_list_scene_epoch)&&
        frame>=q.frame&&frame-q.frame<=8&&atomic_load(&mouse_menu_modal_scene)!=ZA_SHOP_STATE&&
        !mouse_ack_active(ZA_SHOP_STATE,NULL);
}
static void za_shop_quantity_layout(uint64_t layout){
    if(za_menu_state()!=ZA_SHOP_STATE)return;
    uint64_t pane=za_ui_layout_pane(layout,"L_selectnum_00");
    if(!pane)return;
    ZaShopQuantity q={.layout=layout,.frame=renderer_present_count(),.epoch=atomic_load(&mouse_list_scene_epoch)};
    bool shown=za_ui_visible(pane);
    q.shown=shown;
    if(shown){
        bool up=za_ui_rect(za_ui_layout_pane(layout,"L_selectnum_00/L_arrow_00/N_arrow_00"),&q.up);
        bool down=za_ui_rect(za_ui_layout_pane(layout,"L_selectnum_00/L_arrow_00/N_arrow_01"),&q.down);
        bool confirm=za_ui_rect(za_ui_layout_pane(layout,"L_selectnum_00/L_key_00"),&q.confirm);
        if(!confirm)confirm=za_ui_rect(za_ui_layout_pane(layout,"L_selectnum_00/T_num_00"),&q.confirm);
        q.valid=up&&down&&confirm;
    }
    pthread_mutex_lock(&za_shop_quantity_lock);
    bool accepted=shown||za_shop_quantity.layout==layout;
    bool opened=accepted&&shown&&(!za_shop_quantity.shown||za_shop_quantity.epoch!=q.epoch);
    bool changed=accepted&&(za_shop_quantity.layout!=layout||za_shop_quantity.shown!=q.shown||za_shop_quantity.valid!=q.valid);
    if(accepted)za_shop_quantity=q;
    pthread_mutex_unlock(&za_shop_quantity_lock);
    // The item click opened this popup. Release its confirm before the new
    // modal can receive another input, and discard background hover requests.
    if(opened){za_mouse_ui_cancel();direct_list_cancel();}
    if(changed&&getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-shop-quantity-layout] frame=%llu shown=%d bounds=%d\n",
        (unsigned long long)q.frame,q.shown,q.valid);
}
static int za_shop_quantity_hit(float x,float y,int w,int h){
    if(!za_shop_quantity_active()||w<=0||h<=0||!isfinite(x)||!isfinite(y))return -2;
    float scale=fminf(w/1920.0f,h/1080.0f);x=(x-(w-1920*scale)/2)/scale;y=(y-(h-1080*scale)/2)/scale;
    pthread_mutex_lock(&za_shop_quantity_lock);ZaShopQuantity q=za_shop_quantity;pthread_mutex_unlock(&za_shop_quantity_lock);
    if(!q.valid)return -2;
    ZaUiRect arrows[2]={q.up,q.down};int hit=-2;float distance=INFINITY;bool arrow=false;
    for(unsigned i=0;i<2;i++){
        ZaUiRect r=arrows[i];float cx=r.x+r.width/2,cy=r.y+r.height/2;
        float hw=fmaxf(r.width/2,18),hh=fmaxf(r.height/2,14);
        if(fabsf(x-cx)>hw||fabsf(y-cy)>hh)continue;
        arrow=true;
        float d=(x-cx)*(x-cx)+(y-cy)*(y-cy);
        if(d<distance){distance=d;hit=(int)i+1;}else if(d==distance)hit=-2;
    }
    if(arrow)return hit;
    ZaUiRect r=q.confirm;
    return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height?0:-1;
}
static void za_shop_quantity_queue(int action){
    pthread_mutex_lock(&za_shop_quantity_lock);
    if(za_shop_quantity_epoch!=atomic_load(&mouse_list_scene_epoch)){
        za_shop_quantity_head=za_shop_quantity_count=0;za_shop_quantity_epoch=atomic_load(&mouse_list_scene_epoch);
    }
    if(za_shop_quantity_count<32){
        za_shop_quantity_actions[(za_shop_quantity_head+za_shop_quantity_count)%32]=action;za_shop_quantity_count++;
    }
    pthread_mutex_unlock(&za_shop_quantity_lock);
}
static void za_shop_quantity_reset(void){
    pthread_mutex_lock(&za_shop_quantity_lock);
    za_shop_quantity_head=za_shop_quantity_count=0;za_shop_quantity_until=0;za_shop_quantity_down=-2;
    pthread_mutex_unlock(&za_shop_quantity_lock);atomic_store(&za_shop_quantity_buttons,0);
}
static void za_shop_quantity_frame(uint64_t frame){
    if(!za_shop_quantity_active()||menu_input_captured()||lumiose_scroll_menu_open()){
        za_shop_quantity_reset();return;
    }
    pthread_mutex_lock(&za_shop_quantity_lock);
    uint64_t bits=atomic_load(&za_shop_quantity_buttons);
    if(za_shop_quantity_epoch!=atomic_load(&mouse_list_scene_epoch)){
        za_shop_quantity_head=za_shop_quantity_count=0;za_shop_quantity_until=0;bits=0;
    }
    if(bits&&frame>=za_shop_quantity_until){bits=0;za_shop_quantity_until=frame+1;}
    if(!bits&&frame>=za_shop_quantity_until&&za_shop_quantity_count){
        int action=za_shop_quantity_actions[za_shop_quantity_head];za_shop_quantity_head=(za_shop_quantity_head+1)%32;za_shop_quantity_count--;
        bits=1ull<<(action==1?BTN_UP:action==2?BTN_DOWN:action==0?BTN_A:BTN_B);za_shop_quantity_until=frame+2;
        if(getenv("LARECOMP_MOUSE_UI_LOG"))fprintf(stderr,"[mouse-shop-quantity] frame=%llu action=%d\n",(unsigned long long)frame,action);
    }
    pthread_mutex_unlock(&za_shop_quantity_lock);atomic_store(&za_shop_quantity_buttons,bits);
}
#endif
