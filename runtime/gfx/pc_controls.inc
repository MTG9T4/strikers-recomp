/* SPDX-License-Identifier: GPL-3.0-or-later
 * Included by gfx_options.c. Only input bindings and settings; no device lifetime changes. */
#ifndef PC_KEY_STATE
#define PC_KEY_STATE(k) GetAsyncKeyState(k)
#endif
static SRWLOCK pc_lock = SRWLOCK_INIT;
static INIT_ONCE pc_once = INIT_ONCE_STATIC_INIT;
static GfxOptions pc_opt;
static volatile LONG pc_open, pc_scene, pc_quiet_frames;
static volatile LONG pc_epoch;
static unsigned pc_chord[8], pc_release[8];
static SRWLOCK pc_pad_lock=SRWLOCK_INIT;
static unsigned long long pc_pad_buttons[8];
static int pc_pad_lx[8],pc_pad_ly[8];
static BOOL CALLBACK pc_init(PINIT_ONCE once, PVOID a, PVOID *b) {
    (void)once; (void)a; (void)b; pc_opt=*gfx_options();
    int nnpad_kb_vk(int idx);
    for(int action=0;action<24;action++)if(!(pc_opt.keys_set>>action&1)) {int vk=nnpad_kb_vk(action);if(vk)pc_opt.keys[action][0]=vk;}
    pc_opt.keys_set=0xffffff;return TRUE;
}
void pc_settings_get(GfxOptions *out) {
    InitOnceExecuteOnce(&pc_once,pc_init,NULL,NULL);
    AcquireSRWLockShared(&pc_lock); *out=pc_opt; ReleaseSRWLockShared(&pc_lock);
}
void pc_settings_apply(const GfxOptions *o) {
    InitOnceExecuteOnce(&pc_once,pc_init,NULL,NULL);
    AcquireSRWLockExclusive(&pc_lock); pc_opt=*o; ReleaseSRWLockExclusive(&pc_lock);
}
/* The PC menus (pc_overlay.cpp): PC_START over the game's boot, PC_MENU in game (F1 / Minus+Plus), PC_SETTINGS the tabbed
 * settings, opened from either and going back to it. Any of them open: the game gets no pad, key or mouse input. */
static volatile LONG pc_mode, pc_parent, pc_tab;
void pc_menu_set(int mode) {
    LONG old=InterlockedExchange(&pc_mode,mode);
    InterlockedExchange(&pc_open,mode!=PC_CLOSED);
    InterlockedIncrement(&pc_epoch);
    if(old!=mode)printf("pc-settings: menu %ld -> %d\n",old,mode);
}
int pc_menu_mode(void) { return (int)InterlockedCompareExchange(&pc_mode,0,0); }
void pc_open_settings(int online_tab) {
    LONG m=pc_menu_mode();
    if(m!=PC_SETTINGS)InterlockedExchange(&pc_parent,m);
    InterlockedExchange(&pc_tab,online_tab);
    pc_menu_set(PC_SETTINGS);
}
int pc_settings_tab_take(void) { return (int)InterlockedExchange(&pc_tab,0); }
void pc_settings_toggle(void) {
    if(gfx_options()->renderer) {printf("pc-settings: the settings overlay requires Vulkan\n");return;}
    LONG m=pc_menu_mode();
    if(m==PC_CLOSED)pc_menu_set(PC_MENU);
    else if(m==PC_MENU)pc_menu_set(PC_CLOSED);
    else if(m==PC_SETTINGS)pc_menu_set(InterlockedCompareExchange(&pc_parent,0,0));
    /* PC_START stays until Play or Quit */
}
/* display.c, once the window exists: the start screen, for a person at the game. Scripted runs and hidden test copies skip it. */
void pc_start_screen_maybe(int visible) {
    const char *skip[]={"STRIKERS_SDL_TEST_SEQ","STRIKERS_SEQ","STRIKERS_PLAY","STRIKERS_SCRIPT","STRIKERS_KEYBOARD_TEST_SEQ","STRIKERS_PRESS","STRIKERS_NO_START_SCREEN"};
    if(!visible || gfx_options()->renderer || !gfx_options()->start_screen)return;
    for(int i=0;i<7;i++)if(getenv(skip[i]))return;
    pc_menu_set(PC_START);
}
int pc_settings_open(void) { return InterlockedCompareExchange(&pc_open,0,0)!=0; }
int pc_settings_epoch(void) { return (int)InterlockedCompareExchange(&pc_epoch,0,0); }
int pc_input_gate(unsigned id, unsigned long long buttons, int lx, int ly, int rx, int ry) {
    if(id>=8)return pc_settings_open();
    AcquireSRWLockExclusive(&pc_pad_lock);
    pc_pad_buttons[id]=buttons;pc_pad_lx[id]=lx;pc_pad_ly[id]=ly;
    int chord=(buttons & ((1ull<<10)|(1ull<<11)))==((1ull<<10)|(1ull<<11));
    if(chord && !pc_chord[id]) {pc_release[id]=1;pc_settings_toggle();} pc_chord[id]=chord;
    if(pc_settings_open())pc_release[id]=1;
    if(!pc_settings_open() && pc_release[id] && !buttons && abs(lx)<6000 && abs(ly)<6000 && abs(rx)<6000 && abs(ry)<6000)pc_release[id]=0;
    int blocked=pc_settings_open() || pc_release[id];
    ReleaseSRWLockExclusive(&pc_pad_lock);return blocked;
}
void pc_pad_navigation(unsigned long long *buttons,int *lx,int *ly) {
    *buttons=0;*lx=*ly=0;AcquireSRWLockShared(&pc_pad_lock);
    for(int p=0;p<8;p++) {*buttons|=pc_pad_buttons[p];if(abs(pc_pad_lx[p])>abs(*lx))*lx=pc_pad_lx[p];if(abs(pc_pad_ly[p])>abs(*ly))*ly=pc_pad_ly[p];}
    ReleaseSRWLockShared(&pc_pad_lock);
}
int pc_key_override(int action) { GfxOptions o;pc_settings_get(&o);return action>=0 && action<24 && (o.keys_set>>action&1); }
int pc_key_down(int action,int legacy_vk) {
    static unsigned char held[256];
    if(action<0||action>=24)return 0;
    GfxOptions o; pc_settings_get(&o);
    if(pc_settings_open()) { for(int k=1;k<256;k++)held[k]=1; return 0; }
    int *keys=o.keys[action];
    int fallback[3]={legacy_vk,0,0};
    /* Legacy SDL action mappings win until explicitly overridden. Fill the
     * formerly absent right stick/clicks with the complete PC defaults. */
    if(!(o.keys_set>>action&1) && legacy_vk)keys=fallback;
    int down=0;
    for(int i=0;i<3;i++) { int k=keys[i]; if(k<=0||k>=256)continue;
        int on=(PC_KEY_STATE(k)&0x8000)!=0;
        if(!on)held[k]=0;
        if(on && !held[k] && (k!=VK_LBUTTON && k!=VK_RBUTTON || (o.mouse_menu && pc_quiet_frames>=30)))down=1;
    }
    if(o.mouse_menu && pc_quiet_frames>=30 && action<2) {
        int k=action?VK_RBUTTON:VK_LBUTTON, on=(PC_KEY_STATE(k)&0x8000)!=0;
        if(!on)held[k]=0; if(on&&!held[k])down=1;
    }
    return down;
}
void pc_scene_hint(int scene) {
    InterlockedExchange(&pc_scene,scene);
    if(scene)InterlockedExchange(&pc_quiet_frames,0); else InterlockedIncrement(&pc_quiet_frames);
}
int pc_cursor_visible(void) {return pc_settings_open() || !pc_scene;}
void pc_window_geometry(int x,int y,int width,int height) {
    InitOnceExecuteOnce(&pc_once,pc_init,NULL,NULL);
    AcquireSRWLockExclusive(&pc_lock);pc_opt.pos_x=x;pc_opt.pos_y=y;pc_opt.position_valid=1;pc_opt.width=width;pc_opt.height=height;ReleaseSRWLockExclusive(&pc_lock);
}
