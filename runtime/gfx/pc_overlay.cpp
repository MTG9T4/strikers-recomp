/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host settings, rendered at swapchain resolution after the game's final blit.
 * ImGui/backends remain the unmodified pinned dependency in deps/imgui. */
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pc_controls.h"
#include "../services/zt_transport.h"
#include "../services/group_client.h"
#include "../services/relay_transport.h"
#include "../fs/save_import.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.cpp"
#include "backends/imgui_impl_vulkan.cpp"
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"dwmapi.lib")
extern "C" void *display_hwnd(void);
extern "C" void display_set_mode(int mode);
extern "C" int nnaudio_volume(void);
extern "C" void nnaudio_set_volume(int percent);
extern "C" int nnpad_kb_vk(int idx);
extern "C" int bug_report_message(char *out, size_t n);
extern "C" int bug_report_submit(const char *description, int screenshot, int crash);
extern "C" int bug_report_send_state(char *out, size_t n);
extern "C" int bug_report_can_send(void);
extern "C" int bug_report_recent_crash(void);
extern "C" double vk_gpu_ns_total(void);
extern "C" int mods_all(char names[][128], int *on, int max);      /* fs/mods.c */
extern "C" void mods_set(const char *name, int on);
extern "C" void mods_folder(char *out, size_t n);
extern "C" int cheats_list(char names[][96], int *on, int max, char *status, size_t sn);   /* core/cheats.c */
extern "C" void cheats_set(int index, int on);
extern "C" void *g_guest_main_handle;
extern "C" int display_fast_forward(void);
extern "C" const char *oe_language(void);                          /* nnmisc.c: the language the game runs in (read once at start) */
extern "C" int vk_hdr_available(void);                             /* hdr_present.inc: 1 Windows HDR on, 2 test, 0 none */                         /* display.c: the factor while Tab is held, else 1 */

/* ---- the performance readout (F10, or Settings > Graphics): frames a second, and where a frame's time goes: the game's
 * main thread (processor time), the render thread (this thread: translating the game's drawing for Vulkan) and the
 * graphics card (timestamps around each command buffer). What limits the frame rate is named from those. */
static volatile LONG perf_flip;
extern "C" void pc_perf_toggle(void) { InterlockedXor(&perf_flip,1); }
static bool perf_on(void) { GfxOptions o;pc_settings_get(&o);return ((o.overlay!=0) ^ (perf_flip!=0)); }     /* the settings in use, Save or not */
struct PerfView { double fps,game,render,gpu; char limit[96]; ImU32 colour; bool ready; };
static PerfView perf;
static unsigned long long thread_ns(void *h) {
    FILETIME c,e,k,u;if(!h||!GetThreadTimes((HANDLE)h,&c,&e,&k,&u))return 0;
    return ((((unsigned long long)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((unsigned long long)u.dwHighDateTime<<32)|u.dwLowDateTime))*100ull;
}
static void perf_update(void) {
    static LARGE_INTEGER last;static int frames;static double gpu0;static unsigned long long game0,render0;
    LARGE_INTEGER now,fq;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&fq);
    frames++;
    double s=last.QuadPart?(double)(now.QuadPart-last.QuadPart)/(double)fq.QuadPart:0;
    if(last.QuadPart&&s<0.5)return;
    double gpu=vk_gpu_ns_total();unsigned long long game=thread_ns(g_guest_main_handle),render=thread_ns(GetCurrentThread());
    if(last.QuadPart&&frames>0) {
        perf.fps=frames/s;
        perf.game=(game-game0)/1e6/frames;perf.render=(render-render0)/1e6/frames;perf.gpu=(gpu-gpu0)/1e6/frames;
        double interval=1000.0/perf.fps,top=perf.game;const char *who="the processor (the game's main thread)";
        if(perf.render>top){top=perf.render;who="the processor (the render thread)";}
        if(perf.gpu>top){top=perf.gpu;who="the graphics card";}
        if(perf.fps>=57.5){snprintf(perf.limit,sizeof perf.limit,"Limited by: the game's own 60 fps (%.1f ms to spare)",interval-top>0?interval-top:0);perf.colour=IM_COL32(120,220,120,255);}
        else if(top>=0.8*interval){snprintf(perf.limit,sizeof perf.limit,"Limited by: %s",who);perf.colour=IM_COL32(255,160,22,255);}
        else{snprintf(perf.limit,sizeof perf.limit,"Limited by: waiting (loading, or graphics being prepared)");perf.colour=IM_COL32(249,197,0,255);}
        perf.ready=true;
    }
    last=now;frames=0;gpu0=gpu;game0=game;render0=render;
}

static SRWLOCK ui_lock=SRWLOCK_INIT;
static SRWLOCK event_lock=SRWLOCK_INIT;
struct UiEvent { void *hwnd; unsigned msg; unsigned long long w; long long l; int mods; };
static UiEvent events[512];
static unsigned event_count;
static bool ui_ready, renderer_ready;
static VkFormat ui_format;
static float ui_dpi=1;
static volatile LONG capture_action=-1, capture_slot, capture_wait, captured_key;
static volatile LONG group_notification;
static void notify_group_invite(void) {
    /* Audit 06: the invitation toast is drawn with the settings closed too (pc_ui_draw), and the Online tab is selected the
     * next time they are opened. Opening the settings here took the player's pads and keyboard away from the game
     * (pc_input_gate, pc_key_down) whenever a friend invited them, in the middle of a match as well. */
    InterlockedExchange(&group_notification,1);
}
static LARGE_INTEGER ui_last, ui_freq;
static VkDevice ui_device;
static void (*ui_load)(void);
static void *(*host_loader)(void *,const char *);
static VkInstance ui_instance;
static VkImageView ui_views[32];
static int ui_view_cursor;
extern "C" void pc_ui_reset_targets(void) {
    AcquireSRWLockExclusive(&ui_lock);
    if(ui_device)for(int i=0;i<32;i++)if(ui_views[i]) {vkDestroyImageView(ui_device,ui_views[i],nullptr);ui_views[i]=VK_NULL_HANDLE;}
    ReleaseSRWLockExclusive(&ui_lock);
}
static PFN_vkVoidFunction load_vk(const char *name,void *) { return (PFN_vkVoidFunction)host_loader(ui_instance,name); }
static void check_vk(VkResult r) { if(r<0) { printf("pc-overlay: Vulkan failed %d\n",(int)r); ExitProcess(5); } }

/* STRIKERS_UI_TRACE: where the last item was drawn, logged when it moves, so a test can click it at any window size. */
static void ui_item(const char *id) {
    static int on=-1;if(on<0)on=getenv("STRIKERS_UI_TRACE")!=nullptr;
    if(!on)return;
    static char ids[32][40];static int rects[32][4];static int count;
    ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();int r[4]={(int)a.x,(int)a.y,(int)b.x,(int)b.y};
    int i=0;while(i<count&&strcmp(ids[i],id))i++;
    if(i==count){if(count==32)return;snprintf(ids[count++],40,"%s",id);}
    else if(!memcmp(rects[i],r,sizeof r))return;
    memcpy(rects[i],r,sizeof r);printf("pc-ui-item: %s %d %d %d %d\n",id,r[0],r[1],r[2],r[3]);
}
static void key_name(int vk,char *out,size_t n) {
    if(!vk) {snprintf(out,n,"Unbound");return;}
    if(vk==VK_LBUTTON||vk==VK_RBUTTON) {snprintf(out,n,"Mouse %d",vk==VK_LBUTTON?1:2);return;}
    UINT scan=MapVirtualKeyA(vk,MAPVK_VK_TO_VSC);
    if(vk>=VK_PRIOR && vk<=VK_DOWN || vk==VK_INSERT || vk==VK_DELETE)scan|=0x100;
    if(!GetKeyNameTextA((LONG)(scan<<16),out,(int)n))snprintf(out,n,"VK %02X",vk);
}
extern "C" int pc_ui_message(void *hwnd,unsigned msg,unsigned long long w,long long l) {
    group_notify(notify_group_invite);
    /* Window callbacks (SetCapture/ReleaseCapture in particular) can re-enter
     * wndproc. Never take the render/context lock here. */
    /* Releases are passed on with the menus closed as well: a menu closed by a key press (Enter on PLAY) never saw that key
     * come up, and the next press of it did not count (job 33's first test: QUIT could not be chosen after PLAY). */
    bool release=msg==WM_KEYUP||msg==WM_SYSKEYUP||msg==WM_LBUTTONUP||msg==WM_RBUTTONUP||msg==WM_MBUTTONUP;
    if(!pc_settings_open() && release) {
        AcquireSRWLockExclusive(&event_lock);
        if(event_count<512)events[event_count++]={hwnd,msg,w,l,0};
        ReleaseSRWLockExclusive(&event_lock);
    }
    if(pc_settings_open()) {
        if((msg>=WM_KEYFIRST && msg<=WM_KEYLAST) || (msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) || msg==WM_SETFOCUS || msg==WM_KILLFOCUS || msg==WM_CAPTURECHANGED) {
            int mods=((GetKeyState(VK_CONTROL)&0x8000)?1:0)|((GetKeyState(VK_SHIFT)&0x8000)?2:0)|((GetKeyState(VK_MENU)&0x8000)?4:0);
            AcquireSRWLockExclusive(&event_lock);
            if(event_count<512)events[event_count++]={hwnd,msg,w,l,mods};
            ReleaseSRWLockExclusive(&event_lock);
        }
        if(capture_action>=0 && !capture_wait) {
            if(msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN) {
                if(w!=VK_F1 && w!=VK_F11 && w!=VK_MENU && !(w==VK_RETURN && GetAsyncKeyState(VK_MENU)&0x8000))InterlockedExchange(&captured_key,(LONG)w);
            }
            if(msg==WM_LBUTTONDOWN || msg==WM_RBUTTONDOWN)InterlockedExchange(&captured_key,msg==WM_LBUTTONDOWN?VK_LBUTTON:VK_RBUTTON);
        }
    }
    return pc_settings_open() && (msg>=WM_KEYFIRST && msg<=WM_KEYLAST || msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST);
}
extern "C" void pc_ui_shutdown(void) {
    AcquireSRWLockExclusive(&ui_lock);
    if(renderer_ready)ImGui_ImplVulkan_Shutdown();renderer_ready=false;
    if(ui_ready) {ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();}ui_ready=false;
    ReleaseSRWLockExclusive(&ui_lock);
}
extern "C" int pc_ui_needed(void) {
    if(pc_settings_open())return 1;
    if(perf_on())return 1;                                                      /* the performance readout */
    char report[8];if(bug_report_message(report,sizeof report))return 1;      /* F9's note */
    if(display_fast_forward()>1)return 1;                                       /* the fast-forward badge */
    GroupStatus notification;group_status(&notification);
    return notification.toast[0]!=0;
}

/* ---- the start screen and the in-game PC menu. Drawn by us over the game's own picture, in the game's colours as an
 * idea only: no art or text of the game is used. Fonts come from Windows (Segoe UI), so nothing extra is shipped. */
static ImFont *font_title, *font_menu, *font_small, *font_body;
static float font_scale=1;
/* Settings > Display > Language shows each language by its own name. Segoe UI has Latin and Cyrillic; the few Chinese,
 * Japanese and Korean glyphs of those names are merged into the settings font from Windows' own CJK fonts (only those
 * glyphs, so the atlas stays small). first..last: the code points this font file gives. */
static void merge_name_glyphs(ImGuiIO &io, const char *windir, const char *file, float size, unsigned first, unsigned last, ImVector<ImWchar> &ranges) {
    char path[MAX_PATH];snprintf(path,sizeof path,"%s\\Fonts\\%s",windir,file);
    if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES)return;
    ImFontGlyphRangesBuilder b;
    for(int i=0;i<PC_LANGUAGE_COUNT;i++)for(const char *t=pc_languages[i].name;*t;) {
        unsigned char lead=(unsigned char)*t++;int more=lead>=0xF0?3:lead>=0xE0?2:lead>=0xC0?1:0;unsigned c=more?lead&(0x3Fu>>more):lead;
        while(more--&&(*t&0xC0)==0x80)c=c<<6|(*t++&0x3F);
        if(c>=first&&c<=last)b.AddChar((ImWchar)c);
    }
    b.BuildRanges(&ranges);if(ranges.Size<2)return;
    ImFontConfig merge;merge.MergeMode=true;
    io.Fonts->AddFontFromFileTTF(path,size,&merge,ranges.Data);
}
static void load_fonts(ImGuiIO &io, float height) {
    font_scale=height>0?height/1080.0f:1;if(font_scale<0.6f)font_scale=0.6f;
    char windir[MAX_PATH]="C:\\Windows";GetWindowsDirectoryA(windir,MAX_PATH);
    char bold[MAX_PATH],semi[MAX_PATH];
    snprintf(bold,sizeof bold,"%s\\Fonts\\segoeuib.ttf",windir);snprintf(semi,sizeof semi,"%s\\Fonts\\seguisb.ttf",windir);
    ImFontConfig font;font.SizePixels=13*ui_dpi;io.Fonts->AddFontDefault(&font);       /* the settings window's font stays the first one */
    if(GetFileAttributesA(bold)!=INVALID_FILE_ATTRIBUTES) {
        const char *body=GetFileAttributesA(semi)!=INVALID_FILE_ATTRIBUTES?semi:bold;
        font_title=io.Fonts->AddFontFromFileTTF(bold,96*font_scale);
        font_menu=io.Fonts->AddFontFromFileTTF(bold,54*font_scale);
        font_small=io.Fonts->AddFontFromFileTTF(body,26*font_scale);
        static const ImWchar body_ranges[]={0x0020,0x00FF,0x0400,0x04FF,0};      /* Latin-1 and Cyrillic */
        font_body=io.Fonts->AddFontFromFileTTF(body,19*font_scale,nullptr,body_ranges);       /* the settings window */
        static ImVector<ImWchar> han,hangul;                                    /* read when the atlas is built */
        merge_name_glyphs(io,windir,"msyh.ttc",19*font_scale,0x3000,0x9FFF,han);      /* Microsoft YaHei: the Han characters */
        merge_name_glyphs(io,windir,"malgun.ttf",19*font_scale,0xAC00,0xD7AF,hangul); /* Malgun Gothic: Hangul */
    }
}
static const char *release_version(void) {
    static char v[32];static int done;
    if(done)return v;done=1;snprintf(v,sizeof v,"PC");
    char path[MAX_PATH];DWORD n=GetModuleFileNameA(nullptr,path,MAX_PATH);if(!n||n>=MAX_PATH)return v;
    char *slash=strrchr(path,'\\');if(!slash)return v;snprintf(slash+1,MAX_PATH-(slash+1-path),"release-manifest.json");
    FILE *f=fopen(path,"rb");if(!f)return v;
    static char buf[1<<20];size_t got=fread(buf,1,sizeof buf-1,f);fclose(f);buf[got]=0;      /* "version" comes after the file list */
    const char *k=strstr(buf,"\"version\"");if(!k)return v;k=strchr(k+9,'"');if(!k)return v;
    const char *e=strchr(k+1,'"');if(!e||e-k>24)return v;snprintf(v,sizeof v,"Version %.*s",(int)(e-k-1),k+1);return v;
}
static void spaced(ImDrawList *d, ImFont *f, float size, ImVec2 at, ImU32 col, const char *s, float gap) {
    for(const char *c=s;*c;c++) {char one[2]={*c,0};d->AddText(f,size,at,col,one);at.x+=f->CalcTextSizeA(size,1e9f,0,one).x+gap;}
}
/* installer\update\available.json, written by the launcher's update check while the game starts (crash_launcher.cpp). */
static bool update_offer(char *version, size_t n, int *minutes) {
    static double next;static bool have;static char v[24];static int mins;
    double now=ImGui::GetTime();
    if(now>=next) {
        next=now+2;have=false;
        char path[MAX_PATH];DWORD len=GetModuleFileNameA(nullptr,path,MAX_PATH);char *slash=len&&len<MAX_PATH?strrchr(path,'\\'):nullptr;
        if(slash) {
            snprintf(slash+1,MAX_PATH-(slash+1-path),"installer\\update\\available.json");
            FILE *f=fopen(path,"rb");
            if(f) {char buf[256];size_t got=fread(buf,1,sizeof buf-1,f);fclose(f);buf[got]=0;
                const char *k=strstr(buf,"\"version\":\"");const char *m=strstr(buf,"\"minutes\":");
                if(k) {k+=11;const char *e=strchr(k,'"');if(e&&e-k<20) {snprintf(v,sizeof v,"%.*s",(int)(e-k),k);mins=m?atoi(m+10):5;have=true;}}
            }
        }
    }
    if(have) {snprintf(version,n,"%s",v);*minutes=mins;}
    return have;
}
static void request_update(void) {
    char path[MAX_PATH];DWORD len=GetModuleFileNameA(nullptr,path,MAX_PATH);char *slash=len&&len<MAX_PATH?strrchr(path,'\\'):nullptr;
    if(!slash)return;
    snprintf(slash+1,MAX_PATH-(slash+1-path),"installer\\update\\requested");
    FILE *f=fopen(path,"wb");if(f) {fputs("update\n",f);fclose(f);}
    printf("pc-settings: update chosen; the launcher starts it when the game has closed\n");
    PostMessageW((HWND)display_hwnd(),WM_CLOSE,0,0);
}
/* PC_START: Play / Online / Settings / Quit. PC_MENU: Resume / Online / Settings / Quit to desktop. Keyboard, mouse and pad. */
static void draw_main_menu(int mode, unsigned width, unsigned height) {
    static double quit_armed_until;
    double now=ImGui::GetTime();
    bool start=mode==PC_START;
    ImDrawList *bg=ImGui::GetBackgroundDrawList();
    float W=(float)width,H=(float)height,s=H/1080.0f;
    /* The game's own menu colours, measured from its screens (job 34): an ember background (#B42B09 to #BA4E0D), a header bar
     * #1F020A, a footer bar #420814, the chosen item and titles #F9C500 with black text, other items black with white text,
     * values #FFA016. The game stays visible behind, in the ember tint. */
    bg->AddRectFilledMultiColor(ImVec2(0,0),ImVec2(W*0.62f,H),IM_COL32(150,30,8,start?240:225),IM_COL32(180,60,12,start?150:120),IM_COL32(186,78,13,start?150:120),IM_COL32(120,20,8,start?240:225));
    bg->AddRectFilled(ImVec2(W*0.62f,0),ImVec2(W,H),IM_COL32(180,60,12,start?150:120));
    bg->AddRectFilled(ImVec2(0,0),ImVec2(W,64*s),IM_COL32(31,2,10,250));
    bg->AddRectFilled(ImVec2(0,H-64*s),ImVec2(W,H),IM_COL32(66,8,20,250));
    ImFont *tf=font_title?font_title:ImGui::GetFont(),*mf=font_menu?font_menu:ImGui::GetFont(),*sf=font_small?font_small:ImGui::GetFont();
    float ts=font_title?tf->FontSize:52*s,ms=font_menu?mf->FontSize:30*s,ss=font_small?sf->FontSize:16*s;
    float x=96*s,y=150*s;
    if(start) {
        bg->AddText(tf,ts,ImVec2(x+3*s,y+4*s),IM_COL32(31,2,10,200),"MARIO STRIKERS");        /* a drop shadow, as the game's titles have */
        bg->AddText(tf,ts,ImVec2(x,y),IM_COL32(255,255,255,255),"MARIO STRIKERS");
        y+=ts*1.02f;
        spaced(bg,sf,ss*1.25f,ImVec2(x+4*s,y),IM_COL32(249,197,0,255),"BATTLE LEAGUE",9*s);
        y+=ss*1.9f;
        bg->AddRectFilledMultiColor(ImVec2(x+4*s,y),ImVec2(x+420*s,y+3*s),IM_COL32(249,197,0,240),IM_COL32(244,117,0,0),IM_COL32(244,117,0,0),IM_COL32(249,197,0,240));
        y+=16*s;
        spaced(bg,sf,ss*0.8f,ImVec2(x+4*s,y),IM_COL32(255,160,22,255),"PC RECOMPILATION",11*s);
    } else {
        bg->AddText(sf,ss*1.1f,ImVec2(x,16*s),IM_COL32(249,197,0,255),"PC MENU");
        bg->AddText(tf,ts*0.8f,ImVec2(x+3*s,y+4*s),IM_COL32(31,2,10,200),"PC MENU");
        bg->AddText(tf,ts*0.8f,ImVec2(x,y),IM_COL32(255,255,255,255),"PC MENU");
        y+=ts*0.9f;
        bg->AddText(sf,ss,ImVec2(x+4*s,y),IM_COL32(255,224,200,255),"The game keeps running behind this menu.");
    }
    if(start)bg->AddText(sf,ss*1.1f,ImVec2(x,16*s),IM_COL32(249,197,0,255),"WELCOME TO MARIO STRIKERS: BATTLE LEAGUE");
    /* the items: a full-window, invisible ImGui window so keyboard / pad navigation and the mouse work as usual */
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(W,H));
    ImGui::PushStyleColor(ImGuiCol_Header,IM_COL32(0,0,0,0));ImGui::PushStyleColor(ImGuiCol_HeaderHovered,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,IM_COL32(0,0,0,0));ImGui::PushStyleColor(ImGuiCol_NavCursor,IM_COL32(0,0,0,0));
    /* The menu keeps its own cursor (sel): ImGui's keyboard / pad navigation lost the window after Esc in the first test,
     * so Up / Down / Enter / Space and the pad's d-pad, stick and A / B are read here. The mouse selects by hovering. */
    ImGui::Begin("##pcmenu",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNav);
    static int sel, last_mode;
    if(mode!=last_mode) {sel=0;last_mode=mode;}
    char upd_version[24]={0};int upd_minutes=0;
    bool upd=update_offer(upd_version,sizeof upd_version,&upd_minutes);
    int count=upd?5:4;
    if(sel>=count)sel=count-1;
    if(ImGui::IsKeyPressed(ImGuiKey_DownArrow)||ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown))sel=(sel+1)%count;
    if(ImGui::IsKeyPressed(ImGuiKey_UpArrow)||ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp))sel=(sel+count-1)%count;
    bool activate=ImGui::IsKeyPressed(ImGuiKey_Enter,false)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,false)||ImGui::IsKeyPressed(ImGuiKey_Space,false)||ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown,false);
    bool quit_armed=now<quit_armed_until;
    char update_label[48];snprintf(update_label,sizeof update_label,"UPDATE TO %s",upd_version);
    /* items: 0 play / resume, 1 online, 2 settings, 3 quit, 4 update (shown second when there is one) */
    const char *names[5]={start?"PLAY":"RESUME","ONLINE","SETTINGS",quit_armed?"PRESS AGAIN TO QUIT":(start?"QUIT":"QUIT TO DESKTOP"),update_label};
    int order[5]={0,1,2,3,4};if(upd) {order[1]=4;order[2]=1;order[3]=2;order[4]=3;}
    float iy=H*0.50f;
    int chosen=-1;
    ImGui::PushFont(mf);
    for(int slot=0;slot<count;slot++) {
        int i=order[slot];
        ImGui::SetCursorPos(ImVec2(x,iy+slot*ms*1.45f));
        ImGui::PushID(i);
        bool pressed=ImGui::InvisibleButton("##item",ImVec2(560*s,ms*1.2f));
        if(ImGui::IsItemHovered()&&ImGui::GetIO().MouseDelta.x*ImGui::GetIO().MouseDelta.x+ImGui::GetIO().MouseDelta.y*ImGui::GetIO().MouseDelta.y>0)sel=slot;
        if(pressed)sel=slot;
        bool focus=sel==slot;
        ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        ImDrawList *d=ImGui::GetWindowDrawList();
        {   /* the game's slanted bars: yellow for the chosen item, black for the others; a little wider when chosen */
            float slant=(b.y-a.y)*0.35f,grow=focus?40*s:0;
            ImVec2 p0(a.x-28*s,a.y),p1(b.x+grow,a.y-(focus?6*s:0)),p2(b.x+grow-slant,b.y),p3(a.x-28*s-slant,b.y+(focus?4*s:0));
            if(focus)d->AddQuadFilled(ImVec2(p0.x+5*s,p0.y+6*s),ImVec2(p1.x+5*s,p1.y+6*s),ImVec2(p2.x+5*s,p2.y+6*s),ImVec2(p3.x+5*s,p3.y+6*s),IM_COL32(31,2,10,170));
            d->AddQuadFilled(p0,p1,p2,p3,focus?(i==3&&quit_armed?IM_COL32(255,160,22,255):IM_COL32(249,197,0,255)):IM_COL32(0,0,0,215));
        }
        ImU32 col=focus?IM_COL32(0,0,0,255):i==3&&quit_armed?IM_COL32(255,160,22,255):IM_COL32(255,255,255,255);
        d->AddText(mf,ms,ImVec2(a.x,a.y+(b.y-a.y-ms)/2),col,names[i]);
        if(pressed)chosen=i;
        ImGui::PopID();
    }
    if(activate&&chosen<0)chosen=order[sel];
    ImGui::PopFont();
    /* bottom: online status and version; the keys that open the menu in game */
    GroupStatus group;group_status(&group);char line[256];
    if(upd)snprintf(line,sizeof line,"Version %s is ready: choose UPDATE (about %d min). Your saves and settings stay.",upd_version,upd_minutes);
    else if(group.configured&&group.joined)group_connection(line,sizeof line);
    else if(group.configured)snprintf(line,sizeof line,"Online: choose ONLINE to join your group with an invite code.");
    else if(zt_relay_mode())snprintf(line,sizeof line,"Local Wireless: rooms on your network and on the internet, nothing to set up.");
    else if(zt_lan_mode())snprintf(line,sizeof line,"Local Wireless: PCs on the same network see each other's rooms.");
    else snprintf(line,sizeof line,"Online is not set up in strikers.ini.");
    bg->AddText(sf,ss*0.9f,ImVec2(x,H-120*s),IM_COL32(255,224,120,255),line);
    bg->AddText(sf,ss*0.85f,ImVec2(x,H-48*s),IM_COL32(255,255,255,255),start?"In game, F1 or Minus + Plus opens the PC menu.":"Esc, B or F1 goes back to the game.");
    const char *ver=release_version();ImVec2 vs=sf->CalcTextSizeA(ss,1e9f,0,ver);
    bg->AddText(sf,ss,ImVec2(W-vs.x-80*s,H-120*s),IM_COL32(255,255,255,255),ver);
    ImVec2 ps=sf->CalcTextSizeA(ss*0.85f,1e9f,0,"Confirm  A    Back  B");bg->AddText(sf,ss*0.85f,ImVec2(W-ps.x-80*s,H-48*s),IM_COL32(249,197,0,255),"Confirm  A    Back  B");
    if(!start&&(ImGui::IsKeyPressed(ImGuiKey_Escape,false)||ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false)))chosen=0;
    ImGui::End();
    ImGui::PopStyleColor(4);
    if(chosen==0)pc_menu_set(PC_CLOSED);
    else if(chosen==4)request_update();
    else if(chosen==1)pc_open_settings(1);
    else if(chosen==2)pc_open_settings(0);
    else if(chosen==3) {
        if(quit_armed) {quit_armed_until=0;printf("pc-settings: quit chosen in the %s\n",start?"start screen":"PC menu");PostMessageW((HWND)display_hwnd(),WM_CLOSE,0,0);}
        else quit_armed_until=now+3.0;
    }
}
/* The game's window images are sRGB (B8G8R8A8_SRGB): the hardware encodes what is written, so ImGui's colours, which are
 * sRGB values already, came out encoded twice: #1F020A showed as a light purple. They are decoded to linear here first. */
static void srgb_draw_data(VkFormat format) {
    if(format!=VK_FORMAT_B8G8R8A8_SRGB && format!=VK_FORMAT_R8G8B8A8_SRGB && format!=VK_FORMAT_A8B8G8R8_SRGB_PACK32)return;
    static unsigned char lut[256];static bool made;
    if(!made) {for(int i=0;i<256;i++) {double c=i/255.0;c=c<=0.04045?c/12.92:pow((c+0.055)/1.055,2.4);lut[i]=(unsigned char)(c*255.0+0.5);}made=true;}
    ImDrawData *dd=ImGui::GetDrawData();
    for(int n=0;n<dd->CmdListsCount;n++) {
        ImDrawList *l=dd->CmdLists[n];
        for(ImDrawVert &v:l->VtxBuffer) {
            ImU32 c=v.col;
            v.col=(c&0xFF000000u)|((ImU32)lut[(c>>16)&255]<<16)|((ImU32)lut[(c>>8)&255]<<8)|(ImU32)lut[c&255];
        }
    }
}
extern "C" void pc_ui_draw(void *instance,void *gpu,void *device,unsigned family,void *queue,void *cmd,
                           unsigned long long image,int format,unsigned width,unsigned height,void *(*loader)(void *,const char *)) {
    AcquireSRWLockExclusive(&ui_lock);
    group_start();GroupStatus notification;group_status(&notification);
    /* started by anything drawn here (pc_ui_needed): with start_screen = off the readout (F10), F9's note and the
     * fast-forward badge come before any menu is opened */
    if(!ui_ready && !pc_ui_needed()) {ReleaseSRWLockExclusive(&ui_lock);return;}
    if(!ui_ready) {
        host_loader=loader;ui_instance=(VkInstance)instance;ui_device=(VkDevice)device;
        ImGui::CreateContext();ImGuiIO &io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
        ui_dpi=ImGui_ImplWin32_GetDpiScaleForHwnd((HWND)display_hwnd());if(ui_dpi<1)ui_dpi=1;
        RECT client={0,0,0,0};GetClientRect((HWND)display_hwnd(),&client);load_fonts(io,(float)(client.bottom>0?client.bottom:height));
        ImGui::StyleColorsDark();
        {   /* the menus' colours: navy, with the start screen's blue as the accent */
            ImGuiStyle &st=ImGui::GetStyle();ImVec4 *c=st.Colors;
            st.WindowRounding=8;st.FrameRounding=5;st.TabRounding=5;st.GrabRounding=5;st.WindowBorderSize=0;
            #define RGB_(r,g,b,a) ImVec4((r)/255.0f,(g)/255.0f,(b)/255.0f,(a))
            c[ImGuiCol_WindowBg]=RGB_(31,2,10,0.97f);c[ImGuiCol_ChildBg]=RGB_(20,1,6,0.9f);c[ImGuiCol_PopupBg]=RGB_(31,2,10,0.98f);
            c[ImGuiCol_TitleBg]=c[ImGuiCol_TitleBgActive]=RGB_(66,8,20,1);c[ImGuiCol_Border]=RGB_(244,117,0,0.5f);
            c[ImGuiCol_FrameBg]=RGB_(58,8,18,1);c[ImGuiCol_FrameBgHovered]=RGB_(110,24,12,1);c[ImGuiCol_FrameBgActive]=RGB_(160,40,10,1);
            c[ImGuiCol_Button]=RGB_(0,0,0,1);c[ImGuiCol_ButtonHovered]=RGB_(244,117,0,1);c[ImGuiCol_ButtonActive]=RGB_(255,160,22,1);
            c[ImGuiCol_Tab]=RGB_(0,0,0,1);c[ImGuiCol_TabHovered]=RGB_(244,117,0,1);c[ImGuiCol_TabSelected]=RGB_(180,45,9,1);c[ImGuiCol_TabSelectedOverline]=RGB_(249,197,0,1);
            c[ImGuiCol_Header]=RGB_(90,14,16,1);c[ImGuiCol_HeaderHovered]=RGB_(244,117,0,1);c[ImGuiCol_HeaderActive]=RGB_(255,160,22,1);
            c[ImGuiCol_SliderGrab]=c[ImGuiCol_CheckMark]=RGB_(249,197,0,1);c[ImGuiCol_SliderGrabActive]=RGB_(255,160,22,1);
            c[ImGuiCol_Separator]=RGB_(244,117,0,0.6f);c[ImGuiCol_TextSelectedBg]=RGB_(244,117,0,0.45f);c[ImGuiCol_NavCursor]=RGB_(249,197,0,1);
            #undef RGB_
        }
        ImGui::GetStyle().ScaleAllSizes(font_body?font_scale:ui_dpi);ImGui_ImplWin32_Init(display_hwnd());
        ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,load_vk);
        QueryPerformanceFrequency(&ui_freq);ui_ready=true;
        GfxOptions o;pc_settings_get(&o);if(!getenv("STRIKERS_VOLUME"))nnaudio_set_volume(o.volume);
    }
    if(!renderer_ready || ui_format!=(VkFormat)format) {
        if(renderer_ready) {vkDeviceWaitIdle(ui_device);ImGui_ImplVulkan_Shutdown();}
        ui_format=(VkFormat)format;
        ImGui_ImplVulkan_InitInfo info={};info.ApiVersion=VK_API_VERSION_1_3;
        info.Instance=(VkInstance)instance;info.PhysicalDevice=(VkPhysicalDevice)gpu;info.Device=(VkDevice)device;
        info.QueueFamily=family;info.Queue=(VkQueue)queue;info.DescriptorPoolSize=32;
        /* A draw per present; the game has at most eight in-flight slots. */
        info.MinImageCount=2;info.ImageCount=16;info.MSAASamples=VK_SAMPLE_COUNT_1_BIT;info.CheckVkResultFn=check_vk;
        info.UseDynamicRendering=true;info.PipelineRenderingCreateInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        info.PipelineRenderingCreateInfo.colorAttachmentCount=1;info.PipelineRenderingCreateInfo.pColorAttachmentFormats=&ui_format;
        ImGui_ImplVulkan_Init(&info);ImGui_ImplVulkan_CreateFontsTexture();renderer_ready=true;
        /* the atlas is built and keeps its pixels for a later re-init: the font files (CJK ones are 13-20 MB) can go */
        static bool fonts_freed;if(!fonts_freed){ImGui::GetIO().Fonts->ClearInputData();fonts_freed=true;}
    }
    ImGui_ImplVulkan_NewFrame();ImGui_ImplWin32_NewFrame();
    ImGuiIO &io=ImGui::GetIO();LARGE_INTEGER now;QueryPerformanceCounter(&now);
    UiEvent pending[512];unsigned count;
    AcquireSRWLockExclusive(&event_lock);count=event_count;memcpy(pending,events,count*sizeof(UiEvent));event_count=0;ReleaseSRWLockExclusive(&event_lock);
    for(unsigned n=0;n<count;n++) {
        UiEvent &e=pending[n];ImGui_ImplWin32_WndProcHandler((HWND)e.hwnd,e.msg,(WPARAM)e.w,(LPARAM)e.l);
        io.AddKeyEvent(ImGuiMod_Ctrl,(e.mods&1)!=0);io.AddKeyEvent(ImGuiMod_Shift,(e.mods&2)!=0);io.AddKeyEvent(ImGuiMod_Alt,(e.mods&4)!=0);
    }
    io.DeltaTime=ui_last.QuadPart?(float)((double)(now.QuadPart-ui_last.QuadPart)/ui_freq.QuadPart):1.0f/60;ui_last=now;
    io.DisplaySize=ImVec2((float)width,(float)height);
    io.MouseDrawCursor=false;
    unsigned long long buttons;int lx,ly;pc_pad_navigation(&buttons,&lx,&ly);
    io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
    const ImGuiKey nav_keys[]={ImGuiKey_GamepadFaceDown,ImGuiKey_GamepadFaceRight,ImGuiKey_GamepadFaceLeft,ImGuiKey_GamepadFaceUp,ImGuiKey_GamepadL3,ImGuiKey_GamepadR3,ImGuiKey_GamepadL1,ImGuiKey_GamepadR1,ImGuiKey_GamepadL2,ImGuiKey_GamepadR2};
    for(int b=0;b<10;b++)io.AddKeyEvent(nav_keys[b],(buttons>>b&1)!=0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,(buttons>>12&1)||lx<-16000);io.AddKeyEvent(ImGuiKey_GamepadDpadUp,(buttons>>13&1)||ly>16000);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight,(buttons>>14&1)||lx>16000);io.AddKeyEvent(ImGuiKey_GamepadDpadDown,(buttons>>15&1)||ly<-16000);
    ImGui::NewFrame();
    static int seen_epoch=-1;static GfxOptions edit;static bool saved=false;
    bool opened=pc_settings_open()!=0;
    int mode=pc_menu_mode();
    {   /* the start screen keeps the game's boot quiet; any other state has the player's own volume */
        static bool muted;
        if(!getenv("STRIKERS_VOLUME")) {
            if(mode==PC_START&&!muted) {nnaudio_set_volume(0);muted=true;}
            else if(mode!=PC_START&&muted) {GfxOptions o;pc_settings_get(&o);nnaudio_set_volume(o.volume);muted=false;}
        }
    }
    if(mode==PC_START||mode==PC_MENU)draw_main_menu(mode,width,height);
    {   /* a menu that opens starts with no key held (a release can still have gone to another window) */
        static bool was_open;
        if(opened&&!was_open)io.ClearInputKeys();
        was_open=opened;
    }
    if(opened && seen_epoch!=pc_settings_epoch()) {
        seen_epoch=pc_settings_epoch();
        pc_settings_get(&edit);
        int actual_volume=nnaudio_volume();if(actual_volume>=0)edit.volume=actual_volume;
        for(int a=0;a<24;a++)if(!(edit.keys_set>>a&1)) { int vk=nnpad_kb_vk(a);if(vk)edit.keys[a][0]=vk; }
        saved=false;capture_action=-1;
    }
    if(mode==PC_SETTINGS) {
        float dpi=ImGui_ImplWin32_GetDpiScaleForHwnd((HWND)display_hwnd());if(dpi<1)dpi=1;
        if(font_body)dpi=font_scale>dpi?font_scale:dpi;      /* sized for the window, as the menus are: 4K is twice 1080p */
        else io.FontGlobalScale=dpi/ui_dpi;
        if(font_body)ImGui::PushFont(font_body);
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0),ImVec2((float)width,(float)height),IM_COL32(120,20,8,200));
        ImVec2 size(fminf(760*dpi,width-40*dpi),fminf(900*dpi,height-40*dpi));
        ImGui::SetNextWindowPos(ImVec2(((float)width-size.x)/2,((float)height-size.y)/2),ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(size,ImGuiCond_Appearing);
        ImGui::Begin("Settings",nullptr,ImGuiWindowFlags_NoCollapse);
        ImGui::PushItemWidth(fminf(420*dpi,width*0.55f));
        ImGui::Text("%.1f fps  |  %.2f ms/frame",io.Framerate,1000.0f/io.Framerate);
        ImGui::TextWrapped("Resolution, vsync, GPU, monitor, language and pad layout changes require restart. Save before restarting.");
        if(ImGui::BeginTabBar("settings")) {
            int select_online=(InterlockedExchange(&group_notification,0)!=0)|pc_settings_tab_take();
            if(ImGui::BeginTabItem("Online",nullptr,select_online?ImGuiTabItemFlags_SetSelected:0)) {
                GroupStatus group;group_status(&group);
                if(group.configured) {
                    ImGui::SeparatorText("Join your group");
                    static char code[16]={0},name[129]={0};static bool first=true;
                    if(first){snprintf(code,sizeof code,"%s",group.code);snprintf(name,sizeof name,"%s",group.name);first=false;}
                    if(!group.joined) {
                        ImGui::InputText("Invite code",code,sizeof code,ImGuiInputTextFlags_CharsUppercase);
                        ImGui::InputText("Your display name",name,sizeof name);
                        ImGui::BeginDisabled(group.busy||!code[0]||!name[0]);
                        if(ImGui::Button("Join"))group_join(code,name);
                        ImGui::EndDisabled();
                        ImGui::TextUnformatted("Paste the code your organizer sent. Your player ID is sent automatically.");
                    }
                    ImGui::TextWrapped("%s",group.message);
                    char connection[256];group_connection(connection,sizeof connection);ImGui::TextWrapped("%s",connection);
                    if(group.joined) {
                        ImGui::SeparatorText("Friends");
                        ImGui::TextUnformatted(group.reachable?"Your group":"Reconnecting to group service...");
                        bool playing=group.in_match!=0;
                        if(ImGui::Checkbox("Show me as in a match",&playing))group_match(playing);
                        ImGui::TextWrapped("Turn this on when your match starts and off when it ends. Hosting is detected from your Local Wireless room.");
                        for(unsigned i=0;i<group.count;i++) {
                            const GroupFriend &f=group.friends[i];
                            ImGui::PushID(f.node);
                            ImGui::Text("%s%s | %s%s",f.name,!strcmp(f.node,group.self)?" (you)":"",f.status,!strcmp(f.status,"hosting")?" [HOSTING]":"");
                            if(strcmp(f.node,group.self)) {
                                ImGui::SameLine();ImGui::BeginDisabled(!group.hosting||group.busy||!strcmp(f.status,"offline"));
                                if(ImGui::Button("Invite"))group_invite(f.node);ImGui::EndDisabled();
                            }ImGui::PopID();
                        }
                    }
                }
                ZtStatus s;zt_status(&s);
                if(zt_lan_mode()) {
                    ImGui::SeparatorText("Local Wireless");
                    ImGui::TextWrapped("Open Quick Battle > Local Wireless. One player creates a room and the others choose it from the list. No accounts and nothing to set up. Everyone needs the same release version.");
                    if(zt_relay_mode()) {
                        ImGui::TextWrapped("Rooms from PCs on your network and from the internet show up in the same list.");
                        ImGui::SeparatorText("Internet rooms");
                        RelayStatus r;relay_status(&r);
                        static const char *const states[]={"Waiting for Local Wireless","Connecting","Connected","Not reachable"};
                        const char *state=states[r.state>=0&&r.state<4?r.state:0];
                        if(r.state==2&&r.ping_ms>=0)ImGui::Text("%s  |  ping %d ms",state,r.ping_ms);else ImGui::TextUnformatted(state);
                        ImGui::TextWrapped("%s",r.message);
                        ImGui::InputText("Room key",edit.room_key,sizeof edit.room_key);
                        char current[64];relay_room_key(current,sizeof current);
                        ImGui::SameLine();ImGui::BeginDisabled(!strcmp(current,edit.room_key));
                        if(ImGui::Button("Use this key")) {
                            relay_set_room_key(edit.room_key);saved=gfx_options_save(&edit)!=0;
                            printf("pc-settings: room key %s, save %s\n",edit.room_key[0]?"set":"cleared",saved?"OK":"FAILED");
                        }
                        ImGui::EndDisabled();
                        ImGui::TextWrapped("Empty: you see everyone's internet rooms. Friends who all type the same key (any word you agree on) see only each other's rooms. Then search for rooms again.");
                    }
                    bool internet=!_stricmp(edit.network_mode,"internet");
                    if(ImGui::Checkbox("Internet rooms (restart)",&internet))snprintf(edit.network_mode,sizeof edit.network_mode,"%s",internet?"internet":"lan");
                    ImGui::TextWrapped("If Windows asks whether to allow the game on networks, allow it on private networks. That is for rooms on your own network; internet rooms need nothing.");
                    ImGui::TextDisabled("Internet rooms go through the project's relay server. It sees your IP address and the room's game traffic, and keeps no record of IP addresses.");
                }
                else if(!group.configured||ImGui::CollapsingHeader("Connection details")) {
                const char *states[]={"Offline","Joining","Connected","Waiting for authorization","Connection error"};
                ImGui::Text("Status: %s",states[s.state>=0&&s.state<5?s.state:4]);
                ImGui::Text("Network: %s",s.name[0]?s.name:"Not connected");
                char mode[64],network[64];zt_config(mode,sizeof mode,network,sizeof network);
                if(s.network)ImGui::Text("Network ID: %016llx",s.network);else ImGui::Text("Network ID: %s",network[0]?network:"Not configured");
                if(s.node)ImGui::Text("Your player ID: %010llx",s.node);
                ImGui::Text("Virtual address: %s",s.address[0]?s.address:"Waiting for address");
                if(zt_requested()) {if(ImGui::Button("Join group network"))zt_start();}
                else ImGui::TextWrapped("To enable online Local Wireless, set [Network] mode = zerotier and zerotier_network = your group's network ID in strikers.ini, then restart.");
                ImGui::TextWrapped("%s",s.message[0]?s.message:"Offline. Choose a group network in strikers.ini to play together.");
                ImGui::SeparatorText("Peers seen by ZeroTier");
                for(unsigned i=0;i<s.peers;i++) {
                    ImGui::Text("%010llx | %s",s.peer[i].id,s.peer[i].path<0?"Path unknown":s.peer[i].path?"Direct":"Relayed");
                    ImGui::SameLine();if(s.peer[i].latency>=0)ImGui::Text("| %d ms",s.peer[i].latency);else ImGui::TextUnformatted("| Latency unknown");
                }
                ImGui::TextWrapped("A peer seen here may not have joined your room. Anyone can host: open Quick Battle > Local Wireless and create a room. Friends choose that room from the list.");
                }
                ImGui::EndTabItem();
            }
            bool report_tab=ImGui::BeginTabItem("Report");ui_item("tab Report");
            if(report_tab) {
                /* F1 > Report: a report sent to the developer, only when the player presses Send (core/bug_report.c) */
                static char what[1001];static bool shot=true,crash=false,first=true;
                bool recent=bug_report_recent_crash()!=0;
                if(first){crash=recent;first=false;}
                if(!bug_report_can_send()) {
                    ImGui::TextWrapped("Sending reports is turned off in this copy (report_url in strikers.ini). F9 saves a report in userdata\\reports that you can attach to a report on RecompLauncher or GitHub.");
                } else {
                    ImGui::TextWrapped("Something wrong? Send a report to the developer. It helps to say what you were doing and what went wrong.");
                    ImGui::InputTextMultiline("##what",what,sizeof what,ImVec2(-1,110*dpi));
                    ImGui::TextDisabled("What happened? (optional)");
                    ImGui::Checkbox("Include a screenshot of the game",&shot);
                    if(recent)ImGui::Checkbox("Include the last crash report (a memory snapshot of the game when it crashed)",&crash);
                    ImGui::SeparatorText("What is sent");
                    ImGui::TextWrapped("Your description, the game's logs of this run and the one before, your processor, graphics card, Windows version and memory, and your controller and strikers.ini settings with the invite code and room key removed%s%s. Your Windows user name and PC name are taken out of file paths. Nothing is sent until you press Send report.",
                                       shot?", a screenshot":"",recent&&crash?", the crash report":"");
                    char status[400];int state=bug_report_send_state(status,sizeof status);
                    ImGui::BeginDisabled(state==1);
                    if(ImGui::Button("Send report")&&bug_report_submit(what,shot?1:0,recent&&crash?1:0)) {
                        printf("pc-settings: report submitted (screenshot %d, crash %d)\n",shot?1:0,recent&&crash?1:0);
                        what[0]=0;
                        if(shot)pc_settings_toggle();     /* the menu closes, so the screenshot shows the game; the result shows as a note */
                    }
                    ImGui::EndDisabled();ui_item("send report");
                    if(state)ImGui::TextWrapped("%s",status);
                    ImGui::TextDisabled("F9 saves the same report on this PC only.");
                }
                ImGui::EndTabItem();
            }
            bool saves_tab=ImGui::BeginTabItem("Saves");ui_item("tab Saves");
            if(saves_tab) {
                /* Saves from Ryujinx, yuzu, Eden and the other yuzu forks on this PC, or any strkrs.save (a console
                 * backup): checked, staged, and put in place at the next start (runtime/fs/save_import.c). */
                static SaveCandidate found[24];static int count=-1;static char note[200];
                if(count<0)count=save_import_scan(found,24);
                ImGui::TextWrapped("Bring in your progress from an emulator or a backup of your Switch save. Your current save is kept as a backup, and you can switch back to it here.");
                if(save_import_staged()) {
                    ImGui::SeparatorText("Ready");
                    ImGui::TextWrapped("A save is ready. It is used from the next start.");
                    if(ImGui::Button("Restart the game now"))save_import_restart();
                    ui_item("restart");
                    ImGui::SameLine();if(ImGui::Button("Cancel")) {save_import_cancel();note[0]=0;}
                    ui_item("cancel");
                }
                ImGui::SeparatorText("Saves found on this PC");
                if(!count)ImGui::TextWrapped("None found in Ryujinx, yuzu, Eden, Citron, Sudachi, Suyu or Torzu. Use Choose a file for a portable emulator or a console backup.");
                for(int i=0;i<count;i++) {
                    ImGui::PushID(i);
                    ImGui::BeginDisabled(!found[i].valid);
                    if(ImGui::Button(found[i].valid?"Use this save":"Not usable"))save_import_stage(found[i].path,note,sizeof note);
                    {char id[16];snprintf(id,sizeof id,"use %d",i);ui_item(id);}
                    ImGui::EndDisabled();
                    ImGui::SameLine();ImGui::Text("%s  |  %s",found[i].source,found[i].when);
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",found[i].path);
                    ImGui::PopID();
                }
                static bool picking=false;
                if(picking) {
                    char chosen[520];int r=save_import_pick_poll(chosen,sizeof chosen);
                    if(r==1)save_import_stage(chosen,note,sizeof note);
                    if(r>=0)picking=false;
                    ImGui::TextUnformatted("Choose the strkrs.save file in the window that opened...");
                } else if(ImGui::Button("Choose a file...")) {save_import_pick_start();picking=true;}
                ImGui::SameLine();if(ImGui::Button("Look again"))count=save_import_scan(found,24);
                if(note[0])ImGui::TextWrapped("%s",note);
                ImGui::TextWrapped("Emulators keep the save as strkrs.save. In Ryujinx: right-click the game > Open User Save Directory. In yuzu or Eden: right-click > Open Save Data Location.");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Mods")) {
                /* RomFS mods as yuzu and Ryujinx keep them: mods\<name>\romfs\... (fs/mods.c); switched from the next start */
                static char names[64][128];static int on[64],count=-1;
                if(count<0)count=mods_all(names,on,64);
                ImGui::TextWrapped("Mods replace the game's files: put each one in its own folder as mods\\<mod name>\\romfs\\..., the way yuzu and Ryujinx keep them. Changes count from the next start.");
                ImGui::TextColored(ImVec4(1,0.63f,0.09f,1),"For online play, everyone in the room needs the same mods, or none.");
                ImGui::SeparatorText("Mods in this game's folder");
                if(!count)ImGui::TextUnformatted("No mods yet.");
                for(int i=0;i<count;i++){bool v=on[i]!=0;if(ImGui::Checkbox(names[i],&v)){on[i]=v;mods_set(names[i],v);}}
                if(ImGui::Button("Open the mods folder")){char dir[MAX_PATH];mods_folder(dir,sizeof dir);CreateDirectoryA(dir,nullptr);ShellExecuteA(nullptr,"open",dir,nullptr,nullptr,SW_SHOWNORMAL);}
                ImGui::SameLine();if(ImGui::Button("Look again##mods"))count=mods_all(names,on,64);
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Cheats")) {
                static char names[128][96];static int on[128];static char status[200];
                int count=cheats_list(names,on,128,status,sizeof status);
                ImGui::TextWrapped("Cheats in the Atmosphere format: put the file for this version (named by the game's build id) in the cheats folder.");
                ImGui::TextColored(ImVec4(1,0.63f,0.09f,1),"Cheats wait while you are in a Local Wireless room.");
                ImGui::TextWrapped("%s",status);
                ImGui::BeginChild("cheatlist",ImVec2(0,fminf(260*dpi,height*0.3f)),ImGuiChildFlags_Borders);
                for(int i=0;i<count;i++){ImGui::PushID(i);bool v=on[i]!=0;if(ImGui::Checkbox(names[i],&v))cheats_set(i,v);ImGui::PopID();}
                ImGui::EndChild();
                if(ImGui::Button("Open the cheats folder")){char dir[MAX_PATH];DWORD n=GetModuleFileNameA(nullptr,dir,MAX_PATH);char *s=n&&n<MAX_PATH?strrchr(dir,'\\'):nullptr;if(s){strcpy(s,"\\cheats");CreateDirectoryA(dir,nullptr);ShellExecuteA(nullptr,"open",dir,nullptr,nullptr,SW_SHOWNORMAL);}}
                ImGui::EndTabItem();
            }
            bool display_tab=ImGui::BeginTabItem("Display");ui_item("tab Display");
            if(display_tab) {
                {   /* the game's language: nnmisc.c desired_language, asked once by the game at start (it loads that language's files) */
                    const PcLanguage *pick=pc_language(edit.language);
                    auto label=[](const PcLanguage *l,char *out,size_t n) {      /* own name; non-Latin scripts also in English */
                        bool latin=true;for(const unsigned char *t=(const unsigned char *)l->name;*t;t++)if(*t>=0xD0)latin=false;
                        if(latin)snprintf(out,n,"%s",l->name);else snprintf(out,n,"%s (%s)",l->name,l->english);
                    };
                    char preview[96];if(pick)label(pick,preview,sizeof preview);else snprintf(preview,sizeof preview,"Automatic (follows Windows)");
                    if(ImGui::BeginCombo("Language (restart)",preview,ImGuiComboFlags_HeightLarge)) {
                        if(ImGui::Selectable("Automatic (follows Windows)",!pick))edit.language[0]=0;
                        if(!pick)ImGui::SetItemDefaultFocus();
                        for(int i=0;i<PC_LANGUAGE_COUNT;i++) {
                            char name[96];label(&pc_languages[i],name,sizeof name);ImGui::PushID(i);
                            if(ImGui::Selectable(name,pick==&pc_languages[i]))snprintf(edit.language,sizeof edit.language,"%s",pc_languages[i].code);
                            if(pick==&pc_languages[i])ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    ui_item("combo Language");
                    const PcLanguage *now=pc_language(oe_language());char running[96];if(now)label(now,running,sizeof running);else snprintf(running,sizeof running,"%s",oe_language());
                    ImGui::TextWrapped("The language of the game's menus and text. Automatic follows the Windows display language. The game is in %s now; a change counts from the next start (Save settings, then restart the game).",running);
                    const char *env=getenv("STRIKERS_LANGUAGE");
                    if(env&&env[0])ImGui::TextColored(ImVec4(1,0.63f,0.09f,1),"STRIKERS_LANGUAGE=%s is set and wins over this setting.",env);
                }
                ImGui::Combo("Window mode",&edit.window_mode,"Windowed\0Borderless fullscreen\0Exclusive fullscreen\0Borderless window\0");
                {   int uw=edit.ultrawide==1?1:edit.ultrawide==2370?2:edit.ultrawide==3556?3:0;
                    if(ImGui::Combo("Ultrawide (restart)",&uw,"Off (16:9)\0The window's shape\0 21:9\0 32:9\0"))edit.ultrawide=uw==1?1:uw==2?2370:uw==3?3556:0;ui_item("combo Ultrawide");
                    if(edit.ultrawide){int hud=edit.ultrawide_hud;if(ImGui::Combo("Ultrawide interface (restart)",&hud,"Stretched (player markers exact)\0Centred 16:9 (markers drift inward)\0"))edit.ultrawide_hud=hud;}
                    ImGui::TextWrapped("Ultrawide shows more of the stadium at the sides.");}
                ImGui::SameLine();if(ImGui::Button("Apply mode"))display_set_mode(edit.window_mode);
                ImGui::InputInt("Monitor (restart)",&edit.monitor);if(edit.monitor<0)edit.monitor=0;
                ImGui::TextUnformatted("Alt+Enter / F11: fullscreen toggle. Windowed size and position are remembered.");
                ImGui::Combo("Vsync (restart)",&edit.present,"Clock / automatic\0On (FIFO)\0Off\0Mailbox\0");
                int cap=edit.frame_cap==30?0:1;if(ImGui::Combo("Presentation cap",&cap,"30 fps\0 60 fps\0"))edit.frame_cap=cap?60:30;
                ImGui::TextWrapped("The game clock remains at 60 Hz on high refresh displays. The presentation cap does not slow the game clock.");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Graphics")) {
                bool readout=edit.overlay!=0;if(ImGui::Checkbox("Performance readout (F10 shows or hides it in game)",&readout)){edit.overlay=readout;InterlockedExchange(&perf_flip,0);}
                const float scales[]={0.5f,0.75f,1,1.5f,2,0};int si=2;for(int i=0;i<6;i++)if(edit.scale==scales[i])si=i;
                if(ImGui::Combo("Internal resolution (restart)",&si,"0.5x (960 x 540)\0 0.75x (1440 x 810)\0 1x (1920 x 1080)\0 1.5x (2880 x 1620)\0 2x (3840 x 2160)\0Match window at startup\0"))edit.scale=scales[si];
                bool smaa=edit.antialias==1;if(ImGui::Checkbox("SMAA anti-aliasing (smooths jagged edges of the picture)",&smaa))edit.antialias=smaa?1:0;ui_item("check SMAA");
                {   bool hdr=edit.hdr!=0;int avail=vk_hdr_available();
                    if(ImGui::Checkbox("HDR",&hdr))edit.hdr=hdr;ui_item("check HDR");
                    ImGui::SameLine();ImGui::TextDisabled(avail==1?"(this monitor: HDR available)":avail==2?"(test mode)":"(turn on HDR in Windows display settings first)");
                    if(edit.hdr) {ImGui::SliderInt("HDR paper white (nits)",&edit.hdr_paper,80,400);ImGui::SliderInt("HDR peak brightness (nits)",&edit.hdr_peak,200,4000);
                        ImGui::TextWrapped("Paper white sets how bright the picture's whites are; peak sets how far the brightest highlights go. Set peak to your monitor's.");}}
                ImGui::Combo("Final image scaling",&edit.upscale,"Linear (off)\0AMD FSR 1 (EASU + RCAS)\0Nearest\0Integer / nearest\0");
                ImGui::SliderFloat("FSR sharpness",&edit.sharpness,0,1,"%.2f");
                ImGui::TextWrapped("FSR processes the completed image including the game UI. The PC settings are drawn afterwards at window resolution. Match window is a startup choice; resizing it needs restart to change internal resolution.");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Audio / controls")) {
                if(ImGui::SliderInt("Master volume",&edit.volume,0,100))nnaudio_set_volume(edit.volume);
                int speakers=edit.speakers+1;if(ImGui::Combo("Speakers (restart)",&speakers,"Automatic (5.1 when Windows is set to 5.1 or more)\0Stereo\0" "5.1 surround\0"))edit.speakers=speakers-1;
                int layout=edit.pad_layout+1;if(ImGui::Combo("Pad layout (restart)",&layout,"Auto / device profile\0Positional\0Labels\0"))edit.pad_layout=layout-1;
                ImGui::TextWrapped("Positional: the east button is Nintendo A. Labels: the printed A is Nintendo A. Device GUID profiles remain in strikers_controllers.ini; hot-plug stays enabled.");
                {   int ffs[5]={0,2,3,4,8},fi=3;for(int i=0;i<5;i++)if(edit.fast_forward==ffs[i])fi=i;
                    if(ImGui::Combo("Fast-forward (hold Tab)",&fi,"Off\0 2x\0 3x\0 4x\0 8x\0"))edit.fast_forward=ffs[fi];ui_item("combo Fast-forward");
                    ImGui::TextWrapped("Holding Tab runs the game faster, for scenes it does not let you skip. Not in Local Wireless rooms.");}
                ImGui::Combo("Button pictures (restart)",&edit.button_pictures,"Automatic (from the pad)\0Switch (the game's own)\0Xbox\0PlayStation\0");ui_item("combo Button pictures");
                bool mouse=edit.mouse_menu!=0;if(ImGui::Checkbox("Mouse confirm / back in supported menus",&mouse))edit.mouse_menu=mouse;
                ImGui::TextWrapped("Left click confirms; right click goes back. Click support is limited to simple menus; use keyboard controls elsewhere. No mouse-look.");
                if(ImGui::Button("Reset keyboard defaults")) {pc_default_keys(edit.keys);edit.keys_set=0xffffff;saved=false;}
                if(capture_action>=0) {
                    ImGui::Text("Press a key for %s (Escape cancels)",pc_action_names[capture_action]);
                    if(capture_wait) {bool held=false;for(int k=1;k<256;k++)if(GetAsyncKeyState(k)&0x8000)held=true;if(!held)capture_wait=0;}
                    int k=InterlockedExchange(&captured_key,0);
                    if(k) {if(k!=VK_ESCAPE) { edit.keys[capture_action][capture_slot]=k;edit.keys_set|=1u<<capture_action; }capture_action=-1;}
                }
                ImGui::BeginChild("Bindings",ImVec2(0,-70*dpi),ImGuiChildFlags_Borders);
                for(int a=0;a<24;a++) {
                    ImGui::PushID(a);ImGui::Text("%-14s",pc_action_names[a]);
                    for(int b=0;b<3;b++) {ImGui::SameLine();ImGui::PushID(b);char name[64];key_name(edit.keys[a][b],name,sizeof name);
                        if(ImGui::Button(name,ImVec2(105*dpi,0))) {capture_action=a;capture_slot=b;capture_wait=1;captured_key=0;}
                        if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {edit.keys[a][b]=0;edit.keys_set|=1u<<a;}
                        ImGui::PopID();
                    }
                    bool conflict=false;for(int b=0;b<3;b++)if(edit.keys[a][b])for(int x=0;x<24;x++)if(x!=a)for(int y=0;y<3;y++)if(edit.keys[x][y]==edit.keys[a][b])conflict=true;
                    if(conflict) {ImGui::SameLine();ImGui::TextColored(ImVec4(1,0.65f,0.2f,1),"Conflict");}
                    ImGui::PopID();
                }
                ImGui::EndChild();ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        /* Changes to bindings, scaling filter and cap are live; restart fields
         * stay in the pending config and cannot mutate active GPU resources. */
        pc_settings_apply(&edit);
        bool save_pressed=ImGui::Button("Save settings");ui_item("save settings");
        if(save_pressed) {
            GfxOptions geometry;pc_settings_get(&geometry);
            RECT rc,wr;if(GetClientRect((HWND)display_hwnd(),&rc) && GetWindowRect((HWND)display_hwnd(),&wr) && (GetWindowLongA((HWND)display_hwnd(),GWL_STYLE)&WS_CAPTION)) {
                edit.width=rc.right;edit.height=rc.bottom;edit.pos_x=wr.left;edit.pos_y=wr.top;edit.position_valid=1;
            }
            saved=gfx_options_save(&edit)!=0;pc_settings_apply(&edit);
            printf("pc-settings: save %s: %s\n",saved?"OK":"FAILED",edit.path);
        }
        ImGui::SameLine();if(ImGui::Button("Back"))pc_settings_toggle();
        ImGui::SameLine();ImGui::TextUnformatted(saved?"Saved":"Right-click a binding to clear it");
        ImGui::PopItemWidth();
        ImGui::End();
        if(font_body)ImGui::PopFont();
    }
    if(notification.toast[0]) {
        ImGui::SetNextWindowPos(ImVec2(width-20*ui_dpi,20*ui_dpi),ImGuiCond_Always,ImVec2(1,0));
        ImGui::SetNextWindowSize(ImVec2(fminf(430*ui_dpi,width-40*ui_dpi),0));
        ImGui::Begin("Room invitation",nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextWrapped("%s",notification.toast);
        if(opened&&ImGui::Button("Dismiss"))group_dismiss();
        if(!opened)ImGui::TextUnformatted("F1 > Online to dismiss");
        if(opened&&mode!=PC_SETTINGS&&ImGui::Button("Open Online"))pc_open_settings(1);
        ImGui::End();
    }
    bool perf_shown=perf_on();
    if(perf_shown) {
        perf_update();
        if(perf.ready) {
            if(font_body)ImGui::PushFont(font_body);
            ImGui::SetNextWindowPos(ImVec2(width-20*ui_dpi,height*0.075f),ImGuiCond_Always,ImVec2(1,0));
            ImGui::SetNextWindowBgAlpha(0.72f);
            ImGui::Begin("Performance (F10)",nullptr,ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoTitleBar);
            ImGui::Text("%.0f fps  |  %.1f ms a frame",perf.fps,1000.0/perf.fps);
            ImGui::Text("Game %.1f ms   Render %.1f ms   Graphics card %.1f ms",perf.game,perf.render,perf.gpu);
            ImGui::TextColored(ImColor(perf.colour),"%s",perf.limit);
            ImGui::End();
            if(font_body)ImGui::PopFont();
        }
    }
    int ff=display_fast_forward();
    if(ff>1) {                                                                       /* Tab held: the game runs ff times faster */
        if(font_menu)ImGui::PushFont(font_menu);
        ImGui::SetNextWindowPos(ImVec2(width*0.5f,height*0.075f),ImGuiCond_Always,ImVec2(0.5f,0));
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::Begin("Fast-forward",nullptr,ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoTitleBar);
        ImGui::TextColored(ImVec4(0.98f,0.77f,0,1),">> Fast-forward %dx",ff);
        ImGui::End();
        if(font_menu)ImGui::PopFont();
    }
    char report[400];bool report_toast=bug_report_message(report,sizeof report)!=0;
    if(report_toast) {
        ImGui::SetNextWindowPos(ImVec2(20*ui_dpi,height*0.075f),ImGuiCond_Always);      /* under the start screen's header bar */
        ImGui::SetNextWindowSize(ImVec2(fminf(560*ui_dpi,width-40*ui_dpi),0));
        if(font_body)ImGui::PushFont(font_body);
        ImGui::Begin("Report",nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoInputs);
        ImGui::TextWrapped("%s",report);
        ImGui::End();
        if(font_body)ImGui::PopFont();
    }
    ImGui::Render();
    if(opened || notification.toast[0] || report_toast || (perf_shown && perf.ready) || ff>1) {
        VkImageView view;VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image=(VkImage)image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=(VkFormat)format;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check_vk(vkCreateImageView(ui_device,&vi,nullptr,&view));
        /* Caller waits for this overlay submission before the next one. Views
         * are retired there via pc_ui_shutdown? Keep per-image views below. */
        if(ui_views[ui_view_cursor])vkDestroyImageView(ui_device,ui_views[ui_view_cursor],nullptr);ui_views[ui_view_cursor]=view;ui_view_cursor=(ui_view_cursor+1)%32;
        VkRenderingAttachmentInfo ca={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};ca.imageView=view;ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent={width,height};ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&ca;
        srgb_draw_data((VkFormat)format);
        ImGuiImplVulkanFuncs_vkCmdBeginRenderingKHR((VkCommandBuffer)cmd,&ri);ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),(VkCommandBuffer)cmd);ImGuiImplVulkanFuncs_vkCmdEndRenderingKHR((VkCommandBuffer)cmd);
    }
    ReleaseSRWLockExclusive(&ui_lock);
}
