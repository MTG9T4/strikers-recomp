/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/gfx/gfx_options.c (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* Graphics options: config\strikers.ini.
 *
 * Path: STRIKERS_GRAPHICS_CONFIG=<file> when set; else strikers.ini beside the file STRIKERS_CONFIG names (STRIKERS_CONFIG is the
 * path of strikers_controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\strikers.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (STRIKERS_RENDERER, STRIKERS_VK_DEVICE, STRIKERS_VK_PRESENT) win over the file.
 * The strikers_controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"
#include "pc_controls.h"
#include <math.h>

static const char *const k_default_ini =
"; strikers.ini - native PC settings (UTF-8/ASCII, key = value, ;/# comments).\r\n"
"; Runtime edits use the F1 / Minus+Plus overlay. Save settings persists them.\r\n"
"; Renderer, GPU, monitor, vsync, internal resolution and pad layout require restart.\r\n"
"renderer = vulkan\r\n"
"; window: windowed | borderless_fullscreen | fullscreen (exclusive when supported) | borderless_window\r\n"
"window = windowed\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"; monitor: zero-based Windows enumeration. x/y remember windowed position.\r\n"
"monitor = 0\r\n"
"; vsync: clock | on | off | mailbox. STRIKERS_VSYNC_LOCK=0 disables monitor clock locking.\r\n"
"vsync = clock\r\n"
"; Presentation cap 30 or 60; the game clock remains 60 Hz. Higher values become 60.\r\n"
"frame_cap = 60\r\n"
"gpu = auto\r\n"
"console_mode = docked\r\n"
"; resolution_scale: 0.5..2.0 or window (startup 16:9 client fit, bounded to 0.5..2).\r\n"
"; Screen-class scene/depth and bloom targets scale; square shadow maps stay native.\r\n"
"resolution_scale = 1.0\r\n"
"; upscale: linear (off) | fsr (EASU + RCAS) | nearest | integer (nearest whole-number fit)\r\n"
"upscale = linear\r\n"
"; 0..1, larger is sharper. Downscaling uses linear resampling followed by RCAS.\r\n"
"sharpness = 0.2\r\n"
"volume = 100\r\n"
"; pad_layout: auto | positional | labels. GUID-specific device profiles remain in strikers_controllers.ini.\r\n"
"pad_layout = auto\r\n"
"; Mouse1 confirms / Mouse2 backs out in low-draw menus after 30 stable frames; no mouse-look.\r\n"
"mouse_menu = on\r\n"
"; key.ACTION = key1, key2, key3. Names, NUM0..NUM9, or 0xVK. none = unbound.\r\n"
"; Existing SDL keyboard bindings are imported before the first save. Saving writes all 24 actions here.\r\n"
"; Defaults: A Z; B X; X C; Y V; L Q; R E; ZL A; ZR S; PLUS Enter; MINUS Backspace;\r\n"
"; L click Shift; R click Ctrl; D-pad J I L K; L stick arrows; R stick Num4 Num8 Num6 Num2.\r\n"
"; F1, F11 and Alt+Enter are reserved host shortcuts.\r\n"
"anisotropic_filtering = game\r\n"
"shader_wait = hold\r\n"
"profile = custom\r\n"
"unfocused = run\r\n"
"frame_time_overlay = off\r\n"
"; language: auto (the Windows display language) | en-US | en-GB | ja | fr | de | es-419 | es | it | nl | fr-CA | pt | ru | ko\r\n"
"; | zh-Hant | zh-Hans | pt-BR. Used from the next start. STRIKERS_LANGUAGE=<code> wins.\r\n"
"language = auto\r\n"
"\r\n; mode: internet (Local Wireless rooms on the same network and on the internet) | lan (same network only)\r\n"
"; | zerotier (a private group: set group_service and group_code, or zerotier_network) | offline\r\n"
"; room_key: friends who type the same key see only each other's internet rooms; empty = everyone's rooms\r\n"
"[Network]\r\nmode = internet\r\nroom_key = \r\nrelay = \r\nzerotier_network = \r\ngroup_service = \r\ngroup_code = \r\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK g_save_lock = SRWLOCK_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("STRIKERS_PC_CONFIG"), *c = getenv("STRIKERS_CONFIG");
    if (!e || !e[0]) e = getenv("STRIKERS_GRAPHICS_CONFIG");
    if ((!e || !e[0]) && (!c || !c[0]) && getenv("STRIKERS_USERDATA")) { snprintf(out, MAX_PATH, "%s/strikers.ini", getenv("STRIKERS_USERDATA")); return; }
    char tmp[MAX_PATH], exe[MAX_PATH];
    if (e && e[0]) { snprintf(out, MAX_PATH, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '\\'), *s2 = strrchr(tmp, '/');
        if (s2 && (!s || s2 > s)) s = s2;
        if (s) { *s = 0; snprintf(out, MAX_PATH, "%s\\strikers.ini", tmp); } else snprintf(out, MAX_PATH, "strikers.ini");
        return;
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\strikers.ini", exe);
    if (!_fullpath(out, tmp, MAX_PATH)) snprintf(out, MAX_PATH, "%s", tmp);
}
static BOOL CALLBACK opt_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0, migrated = 0;
    memset(o, 0, sizeof *o);
    snprintf(o->network_mode,sizeof o->network_mode,"internet");
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->window_mode = 0; o->sharpness = 0.2f; o->frame_cap = 60; o->volume = 100; o->pad_layout = -1; o->mouse_menu = 1; o->start_screen = 1; o->speakers = -1; o->fast_forward = 4; o->hdr_paper = 200; o->hdr_peak = 400; pc_default_keys(o->keys);
    opt_path(o->path);
    FILE *f = fopen(o->path, "rb");
    if(!f && !getenv("STRIKERS_PC_CONFIG") && !getenv("STRIKERS_GRAPHICS_CONFIG")) {
        char legacy[MAX_PATH];snprintf(legacy,sizeof legacy,"%s",o->path);
        char *slash=strrchr(legacy,'\\'),*slash2=strrchr(legacy,'/');if(slash2 && (!slash || slash2>slash))slash=slash2;
        if(slash)snprintf(slash+1,sizeof legacy-(size_t)(slash+1-legacy),"strikers_graphics.ini");else snprintf(legacy,sizeof legacy,"strikers_graphics.ini");
        f=fopen(legacy,"rb");if(f)migrated=1;
    }
    if (!f) {
        char b[MAX_PATH];
        snprintf(b, sizeof b, "%s", o->path);
        for (char *p = b + 3; *p; p++) if (*p == '\\' || *p == '/') { *p = 0; CreateDirectoryA(b, NULL); *p = '\\'; }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!_stricmp(key, "renderer")) o->renderer = !_stricmp(val, "opengl") || !_stricmp(val, "gl");
        else if (!_stricmp(key, "window")) o->window_mode = !_stricmp(val, "borderless_fullscreen") || !_stricmp(val, "borderless") ? 1 : !_stricmp(val, "fullscreen") ? 2 : !_stricmp(val, "windowed") ? 0 : 3;
        else if (!_stricmp(key, "mode")) snprintf(o->network_mode,sizeof o->network_mode,"%s",val);
        else if (!_stricmp(key, "zerotier_network")) snprintf(o->zerotier_network,sizeof o->zerotier_network,"%s",val);
        else if (!_stricmp(key, "group_service")) snprintf(o->group_service,sizeof o->group_service,"%s",val);
        else if (!_stricmp(key, "group_code")) snprintf(o->group_code,sizeof o->group_code,"%s",val);
        else if (!_stricmp(key, "room_key")) snprintf(o->room_key,sizeof o->room_key,"%s",val);
        else if (!_stricmp(key, "relay")) snprintf(o->relay,sizeof o->relay,"%s",val);
        else if (!_stricmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!_stricmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!_stricmp(key, "vsync")) o->present = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 2 : !_stricmp(val, "mailbox") ? 3 : 0;
        else if (!_stricmp(key, "gpu")) { if (_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!_stricmp(key, "console_mode")) o->handheld = !_stricmp(val, "handheld");
        else if (!_stricmp(key, "resolution_scale")) { float v = (float)atof(val); if (!_stricmp(val, "window")) o->scale = 0; else if (isfinite(v) && v > 0) o->scale = v; }
        else if (!_stricmp(key, "upscale")) o->upscale = !_stricmp(val,"fsr") ? 1 : !_stricmp(val,"nearest") ? 2 : !_stricmp(val,"integer") ? 3 : 0;
        else if (!_stricmp(key, "sharpness")) { float v=(float)atof(val); if(isfinite(v)) o->sharpness=fmaxf(0,fminf(1,v)); }
        else if (!_stricmp(key, "monitor")) o->monitor = atoi(val) >= 0 ? atoi(val) : 0;
        else if (!_stricmp(key, "x")) { o->pos_x=atoi(val); o->position_valid=1; }
        else if (!_stricmp(key, "y")) o->pos_y=atoi(val);
        else if (!_stricmp(key, "frame_cap")) { int v=atoi(val); o->frame_cap=v==30 ? 30 : 60; }
        else if (!_stricmp(key, "volume")) { int v=atoi(val); o->volume=v<0?0:v>100?100:v; }
        else if (!_stricmp(key, "pad_layout")) o->pad_layout= !_stricmp(val,"positional")?0:!_stricmp(val,"labels")?1:-1;
        else if (!_stricmp(key, "mouse_menu")) o->mouse_menu= !_stricmp(val,"on") || !strcmp(val,"1");
        else if (!_strnicmp(key,"key.",4)) {
            for(int a=0;a<24;a++) if(!_stricmp(key+4,pc_action_names[a])) {
                memset(o->keys[a],0,sizeof o->keys[a]); o->keys_set|=1u<<a;
                char *part=val; for(int b=0;b<3 && part;b++) { char *next=strchr(part,','); if(next)*next++=0; trim(part); o->keys[a][b]=pc_parse_key(part); part=next; }
                break;
            }
        }
        else if (!_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!_stricmp(key, "frame_time_overlay")) o->overlay = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "profile")) o->profile = !_stricmp(val, "low");
        else if (!_stricmp(key, "shader_wait")) o->shader_skip = !_stricmp(val, "skip");
        else if (!_stricmp(key, "unfocused")) o->pause_unfocused = !_stricmp(val, "pause");
        else if (!_stricmp(key, "start_screen")) o->start_screen = _stricmp(val, "off") && strcmp(val, "0");
        else if (!_stricmp(key, "speakers")) o->speakers = !_stricmp(val, "stereo") ? 0 : !strcmp(val, "5.1") || !_stricmp(val, "surround") ? 1 : -1;
        else if (!_stricmp(key, "hdr")) o->hdr = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "hdr_paper_white")) { int v = atoi(val); o->hdr_paper = v < 80 ? 80 : v > 400 ? 400 : v; }
        else if (!_stricmp(key, "hdr_peak")) { int v = atoi(val); o->hdr_peak = v < 200 ? 200 : v > 4000 ? 4000 : v; }
        else if (!_stricmp(key, "button_pictures")) o->button_pictures = !_stricmp(val, "switch") || !_stricmp(val, "nintendo") ? 1 : !_stricmp(val, "xbox") ? 2 : !_stricmp(val, "playstation") || !_stricmp(val, "ps") ? 3 : 0;
        else if (!_stricmp(key, "ultrawide_hud")) o->ultrawide_hud = !_stricmp(val, "centred") || !_stricmp(val, "centered");
        else if (!_stricmp(key, "ultrawide")) o->ultrawide = !_stricmp(val, "window") ? 1 : !strcmp(val, "21:9") ? 2370 : !strcmp(val, "32:9") ? 3556 : !strcmp(val, "16:10") ? 0 : 0;
        else if (!_stricmp(key, "antialiasing")) o->antialias = !_stricmp(val, "smaa");
        else if (!_stricmp(key, "language")) { const PcLanguage *l = pc_language(val); snprintf(o->language, sizeof o->language, "%s", l ? l->code : ""); if (!l && _stricmp(val, "auto") && val[0]) unknown++; }
        else if (!_stricmp(key, "fast_forward")) { int v = atoi(val); o->fast_forward = v == 2 || v == 3 || v == 4 || v == 8 ? v : 0; }
        else unknown++;
    }
    if (f) fclose(f);
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    if (o->pad_layout >= 0 && !getenv("STRIKERS_PAD_LAYOUT")) _putenv_s("STRIKERS_PAD_LAYOUT",o->pad_layout ? "label" : "positional");
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of strikers.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[4] = {"windowed", "borderless_fullscreen", "fullscreen", "borderless_window"}, *const k_vs[4] = {"clock", "on", "off", "mailbox"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d unfocused=%s vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height,
           o->pause_unfocused ? "pause" : "run", k_vs[o->present], o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off",
           unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale != 0 && (o->scale < 0.5f || o->scale > 2.0f))) printf("graphics options: resolution_scale=%.2f is outside 0.5 .. 2.0: the nearest of the two is used\n", o->scale);
    if(migrated)printf("graphics options: legacy graphics imported; canonical save %s\n",gfx_options_save(o)?"OK":"FAILED");
    fflush(stdout);
    return TRUE;
}
const GfxOptions *gfx_options(void) {
    InitOnceExecuteOnce(&g_once, opt_load, NULL, NULL);
    return &g_opt;
}

/* The game's languages, in title-entry order. Names are UTF-8 (pc_overlay.cpp load_fonts loads the glyphs they need). */
const PcLanguage pc_languages[PC_LANGUAGE_COUNT] = {
    {"en-US", "English (US)", "American English"},
    {"en-GB", "English (UK)", "British English"},
    {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "Japanese"},
    {"fr", "Fran\xC3\xA7" "ais (France)", "French"},
    {"de", "Deutsch", "German"},
    {"es-419", "Espa\xC3\xB1ol (Latinoam\xC3\xA9rica)", "Latin American Spanish"},
    {"es", "Espa\xC3\xB1ol (Espa\xC3\xB1" "a)", "Spanish"},
    {"it", "Italiano", "Italian"},
    {"nl", "Nederlands", "Dutch"},
    {"fr-CA", "Fran\xC3\xA7" "ais (Canada)", "Canadian French"},
    {"pt", "Portugu\xC3\xAAs (Portugal)", "Portuguese"},
    {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", "Russian"},
    {"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", "Korean"},
    {"zh-Hant", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", "Traditional Chinese"},
    {"zh-Hans", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", "Simplified Chinese"},
    {"pt-BR", "Portugu\xC3\xAAs (Brasil)", "Brazilian Portuguese"},
};
const PcLanguage *pc_language(const char *code) {
    if (code && code[0]) for (int i = 0; i < PC_LANGUAGE_COUNT; i++) if (!_stricmp(code, pc_languages[i].code)) return &pc_languages[i];
    return NULL;
}

/* Canonical PC configuration. Legacy device GUID profiles stay owned by SDL. */
const char *const pc_action_names[24] = {"A","B","X","Y","STICK_L","STICK_R","L","R","ZL","ZR","PLUS","MINUS","LEFT","UP","RIGHT","DOWN","LSTICK_LEFT","LSTICK_UP","LSTICK_RIGHT","LSTICK_DOWN","RSTICK_LEFT","RSTICK_UP","RSTICK_RIGHT","RSTICK_DOWN"};
void pc_default_keys(int keys[24][3]) {
    static const int k[24]={'Z','X','C','V',VK_SHIFT,VK_CONTROL,'Q','E','A','S',VK_RETURN,VK_BACK,'J','I','L','K',VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN,VK_NUMPAD4,VK_NUMPAD8,VK_NUMPAD6,VK_NUMPAD2};
    memset(keys,0,24*3*sizeof(int)); for(int a=0;a<24;a++) keys[a][0]=k[a];
}
int pc_parse_key(const char *s) {
    if(!s || !*s || !_stricmp(s,"none")) return 0;
    if(!s[1] && ((s[0]>='a' && s[0]<='z') || (s[0]>='A' && s[0]<='Z') || (s[0]>='0' && s[0]<='9'))) return (s[0]>='a' && s[0]<='z')?s[0]-32:s[0];
    static const char *names[]={"LEFT","UP","RIGHT","DOWN","ENTER","BACKSPACE","SPACE","TAB","ESC","SHIFT","CTRL","ALT","MOUSE1","MOUSE2"};
    static const int codes[]={VK_LEFT,VK_UP,VK_RIGHT,VK_DOWN,VK_RETURN,VK_BACK,VK_SPACE,VK_TAB,VK_ESCAPE,VK_SHIFT,VK_CONTROL,VK_MENU,VK_LBUTTON,VK_RBUTTON};
    for(int i=0;i<sizeof codes/sizeof codes[0];i++) if(!_stricmp(s,names[i]))return codes[i];
    if(!_strnicmp(s,"NUM",3) && s[3]>='0' && s[3]<='9' && !s[4])return VK_NUMPAD0+s[3]-'0';
    if((s[0]=='F'||s[0]=='f') && atoi(s+1)>=1 && atoi(s+1)<=24)return VK_F1+atoi(s+1)-1;
    char *end; long v=strtol(s,&end,0); return !*end && v>0 && v<256 ? (int)v:0;
}
int gfx_options_save(const GfxOptions *o) {
    AcquireSRWLockExclusive(&g_save_lock);
    char tmp[MAX_PATH+8]; snprintf(tmp,sizeof tmp,"%s.tmp",o->path);
    FILE *f=fopen(tmp,"wb"); if(!f) {ReleaseSRWLockExclusive(&g_save_lock);return 0;}
    static const char *win[]={"windowed","borderless_fullscreen","fullscreen","borderless_window"}, *vs[]={"clock","on","off","mailbox"}, *up[]={"linear","fsr","nearest","integer"};
    fprintf(f,"; strikers.ini - PC settings. key.ACTION accepts up to three comma-separated Windows VK codes.\n; Resolution, renderer, GPU, present mode, monitor and controller layout require restart.\n; frame_cap is 30 or 60; logic clock remains 60 Hz. F1 or Minus+Plus opens settings.\n");
    fprintf(f,"renderer = %s\nwindow = %s\nwidth = %d\nheight = %d\nmonitor = %d\n",o->renderer?"opengl":"vulkan",win[o->window_mode&3],o->width,o->height,o->monitor);
    if(o->position_valid)fprintf(f,"x = %d\ny = %d\n",o->pos_x,o->pos_y);
    if(o->scale==0)fprintf(f,"resolution_scale = window\n");else fprintf(f,"resolution_scale = %.4g\n",o->scale);
    fprintf(f,"vsync = %s\ngpu = %s\nconsole_mode = %s\nupscale = %s\nsharpness = %.4g\nframe_cap = %d\nvolume = %d\npad_layout = %s\nmouse_menu = %s\n",vs[o->present&3],o->gpu[0]?o->gpu:"auto",o->handheld?"handheld":"docked",up[o->upscale&3],o->sharpness,o->frame_cap,o->volume,o->pad_layout<0?"auto":o->pad_layout?"labels":"positional",o->mouse_menu?"on":"off");
    fprintf(f,"anisotropic_filtering = %d\nshader_wait = %s\nprofile = custom\nunfocused = %s\nframe_time_overlay = %s\nstart_screen = %s\nspeakers = %s\n",o->aniso,o->shader_skip?"skip":"hold",o->pause_unfocused?"pause":"run",o->overlay?"on":"off",o->start_screen?"on":"off",o->speakers==0?"stereo":o->speakers==1?"5.1":"auto");
    fprintf(f,"ultrawide_hud = %s\n",o->ultrawide_hud?"centred":"stretched");
    fprintf(f,"ultrawide = %s\n",o->ultrawide==1?"window":o->ultrawide==2370?"21:9":o->ultrawide==3556?"32:9":"off");
    { static const char *const bp[4] = {"auto", "switch", "xbox", "playstation"}; fprintf(f,"button_pictures = %s\n",bp[o->button_pictures&3]); }
    fprintf(f,"antialiasing = %s\nhdr = %s\nhdr_paper_white = %d\nhdr_peak = %d\n",o->antialias==1?"smaa":"off",o->hdr?"on":"off",o->hdr_paper,o->hdr_peak);
    if(o->fast_forward)fprintf(f,"fast_forward = %d\n",o->fast_forward);else fprintf(f,"fast_forward = off\n");
    fprintf(f,"; language: auto (the Windows display language) or a code: en-US en-GB ja fr de es-419 es it nl fr-CA pt ru ko zh-Hant zh-Hans pt-BR (next start)\n"
              "language = %s\n",pc_language(o->language)?pc_language(o->language)->code:"auto");
    for(int a=0;a<24;a++)if(o->keys_set>>a&1)fprintf(f,"key.%s = 0x%02X, 0x%02X, 0x%02X\n",pc_action_names[a],o->keys[a][0],o->keys[a][1],o->keys[a][2]);
    fprintf(f,"\n; mode: internet (Local Wireless rooms on the same network and on the internet) | lan (same network only)\n"
              "; | zerotier (a private group: set group_service and group_code, or zerotier_network) | offline\n"
              "; room_key: friends who type the same key see only each other's internet rooms; empty = everyone's rooms\n");
    fprintf(f,"[Network]\nmode = %s\nroom_key = %s\nrelay = %s\nzerotier_network = %s\n",o->network_mode,o->room_key,o->relay,o->zerotier_network);
    fprintf(f,"group_service = %s\ngroup_code = %s\n",o->group_service,o->group_code);
    int ok=!ferror(f); if(fflush(f))ok=0; if(fclose(f))ok=0;
    if(!ok || !MoveFileExA(tmp,o->path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){ DeleteFileA(tmp); ReleaseSRWLockExclusive(&g_save_lock);return 0; } ReleaseSRWLockExclusive(&g_save_lock);return 1;
}

#include "pc_controls.inc"
