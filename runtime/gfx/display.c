/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/gfx/display.c (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* Host display: a real window, 60 Hz pacing and the guest's vsync event.
 * The window's contents are drawn by the graphics layer's render thread (nvn_gl.c's front end; Vulkan by default). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include "host_cpu.h"
#include "hostrt.h"
#include "gfx_options.h"
#include "pc_controls.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define WIDTH 1280
#define HEIGHT 720
#define FRAME_NS 16666667LL

void os_init_event(uint64_t key, int autoclear);      /* os_threads.c */
void os_signal_event(uint64_t key);

static HWND g_hwnd;
static volatile LONG g_ready;
static volatile LONG64 g_frames;
void nvn_gl_start(void *hwnd);                        /* nvn_gl.c */
void nvn_gl_report(FILE *out);
void nvn_tl(int ev, int arg);                          /* nvn_gl.c: STRIKERS_FRAMETL timeline */
static uint64_t g_vsync_event;
static LARGE_INTEGER g_freq;

/* ONE refresh clock. The game counts the signals of the vsync event on a thread of its own ("VSync Count
 * Thread", 0x7103810a40: window object + 0x138) and, at every present (0x710386fc30), advances its frame counter
 * (0x710593a440) by the number of refreshes counted since the previous present. So the refresh count IS the game's
 * time. Before this, the event came from a Sleep() loop and presentation was held by a second, independent 60 Hz
 * clock that restarted its phase after every late frame: between two presents the game counted 0, 1 or 2 refreshes,
 * and "0" counts as one step while "2" counts as two, so the game's clock gained (10.77 s per 600 frames; measured by
 * ssbu-recomp in Super Smash Bros. Ultimate, whose main the addresses above are in).
 * Now: refresh n happens at t0 + n/60 s; at each one the vsync event is signalled and then the frames waiting to be
 * shown are released. A frame queued between refresh n and n+1 is shown at n+1, and no sooner than `interval`
 * refreshes after the frame before it (nvnWindowSetPresentInterval); the game is held in its wait for the window
 * texture (nvnSyncWait on the sync of nvnWindowAcquireTexture), which is AFTER it has read the refresh count, as on
 * the console. STRIKERS_OLD_PACING=1: the two clocks as before. STRIKERS_CLOCKSTAT=1: one line every 600 presented frames. */
static int g_old_pacing = -1, g_clockstat;
static volatile LONG64 g_vsync_n;            /* refreshes passed (published after the event was signalled) */
static volatile LONG64 g_vsync_signals, g_vsync_late;
static SRWLOCK g_vsync_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_vsync_cv = CONDITION_VARIABLE_INIT;
static LONG64 g_latch;                       /* refresh at which the last queued frame is shown */
static volatile LONG g_latch_pending;        /* a frame was queued and nobody has waited for its refresh yet */
static int old_pacing(void) {
    if (g_old_pacing < 0) { const char *e = getenv("STRIKERS_OLD_PACING"); g_old_pacing = e && atoi(e) != 0; g_clockstat = getenv("STRIKERS_CLOCKSTAT") && atoi(getenv("STRIKERS_CLOCKSTAT")) != 0; }
    return g_old_pacing;
}
static void vsync_test_key(void);

/* Removed upstream game feature. */
static volatile LONG g_menu_hold;
void display_menu_hold(int on) { InterlockedExchange(&g_menu_hold, on); }
int display_menu_held(void) { return g_menu_hold != 0; }     /* nvn_vk_watchdog.c: a game held on purpose (menu, guide) is not frozen */
static int vsync_signal_ok(void) { return !g_menu_hold; }

/* ---- the game's own art. This project ships none: setup reads the icon from the player's own dump and makes the icon file
 * and the start-up picture from it (installer/game_art.py) into build\game-art next to this program. Missing: plain window,
 * text start-up screen. */
static int game_art_path(wchar_t *out, DWORD n, const wchar_t *name) {
    DWORD len = GetModuleFileNameW(NULL, out, n);
    wchar_t *slash = len && len < n ? wcsrchr(out, L'\\') : NULL;
    if (!slash || (size_t)(slash - out) + 18 + wcslen(name) >= n) return 0;
    wcscpy(slash + 1, L"build\\game-art\\"); wcscat(out, name);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* ---- the start-up picture (build\game-art\splash.jpg), decoded once with WIC into a 32-bit DIB and drawn to cover the window
 * above the status line's maroon bar, until the game's first frame. 0: no picture (the text splash is drawn). */
#define COBJMACROS
#include <wincodec.h>
static HBITMAP g_splash_bmp; static int g_splash_w, g_splash_h, g_splash_tried;
static void splash_load(void) {
    wchar_t path[MAX_PATH];
    if (!game_art_path(path, MAX_PATH, L"splash.jpg")) return;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *f = NULL; IWICBitmapDecoder *d = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, hgt = 0; IWICFormatConverter_GetSize(cv, &w, &hgt);
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = (LONG)w; bi.bmiHeader.biHeight = -(LONG)hgt; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void *bits = NULL; HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (b && bits && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * 4 * hgt, (BYTE *)bits))) { g_splash_bmp = b; g_splash_w = (int)w; g_splash_h = (int)hgt; }
        else if (b) DeleteObject(b);
    }
    if (cv) IWICFormatConverter_Release(cv); if (fr) IWICBitmapFrameDecode_Release(fr); if (d) IWICBitmapDecoder_Release(d);
    if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(co)) CoUninitialize();
}
static int splash_paint(HWND h) {
    if (!g_splash_tried) { g_splash_tried = 1; splash_load(); }
    if (!g_splash_bmp) return 0;
    static ULONGLONG t0; if (!t0) t0 = GetTickCount64();
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom > 0 ? rc.bottom : 720, bar = H * 64 / 1080;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), maroon = CreateSolidBrush(RGB(66, 8, 20));
    FillRect(dc, &rc, white);
    int area = H - bar, dw = W, dh = W * g_splash_h / g_splash_w;            /* covers the window above the bar: the picture */
    if (dh < area) { dh = area; dw = area * g_splash_w / g_splash_h; }       /* is wide, so only its blurred edges are cut */
    HDC mem = CreateCompatibleDC(dc); HGDIOBJ oldb = SelectObject(mem, g_splash_bmp);
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, (W - dw) / 2, (area - dh) / 2, dw, dh, mem, 0, 0, g_splash_w, g_splash_h, SRCCOPY);
    SelectObject(mem, oldb); DeleteDC(mem);
    RECT b = rc; b.top = rc.bottom - bar; FillRect(dc, &b, maroon);
    DeleteObject(white); DeleteObject(maroon);
    HFONT font = CreateFontW(-(H * 30 / 1080), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT);
    unsigned seconds = (unsigned)((GetTickCount64() - t0) / 1000);
    wchar_t line[96];
    if (seconds < 4) swprintf(line, 96, L"Starting\u2026");
    else swprintf(line, 96, L"Preparing graphics\u2026 %u s (longer the first time, or after a driver update)", seconds);
    SetTextColor(dc, RGB(255, 255, 255)); TextOutW(dc, H * 96 / 1080, rc.bottom - bar + (bar - H * 30 / 1080) / 2 - 2, line, (int)wcslen(line));
    SetTextColor(dc, RGB(249, 197, 0)); { const wchar_t *t = L"MARIO STRIKERS: BATTLE LEAGUE"; SIZE sz; GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        TextOutW(dc, W - sz.cx - H * 96 / 1080, rc.bottom - bar + (bar - H * 30 / 1080) / 2 - 2, t, (int)wcslen(t)); }
    SelectObject(dc, oldf); DeleteObject(font);
    EndPaint(h, &ps);
    return 1;
}

/* TEST ONLY, STRIKERS_SPEED=<n>: the refresh clock (vsync_thread) runs n times faster, 60 x n refreshes a second (any number
 * > 0; 0 = 960 a second, as fast as the PC can run the game). The game waits for a refresh before each frame and steps
 * once per frame, so it runs up to n times faster where the PC keeps up; input sequences count frames, so they play the
 * same. Not set or 1 = nothing changes. (A lockstep clock, a refresh only once the game has queued a frame, never starts
 * this game: it waits for a refresh before its first frame.) */
static double g_rate = 60.0;                          /* refreshes a second */
static void speed_init(void) {
    const char *e = getenv("STRIKERS_SPEED");
    if (!e || !e[0]) return;
    double speed = atof(e);
    g_rate = speed > 0 ? 60.0 * speed : 960.0;
    if (g_rate != 60.0) printf("display: STRIKERS_SPEED=%s (TEST ONLY): the refresh clock runs at %.0f Hz\n", e, g_rate);
}

/* Fast-forward (Settings > Audio / controls, strikers.ini fast_forward = off|2|3|4|8): while Tab is held with the game's
 * window in front, the refresh clock runs that many times faster. The refresh count is the game's time, so the game runs that
 * much faster where the PC keeps up; where it does not, the game still steps by the refreshes it counted (frames are left
 * out, not time). It is the way past scenes the game does not let a player skip, the celebration after a match among them.
 * Never in a Local Wireless room (every player must step together) and never with the PC menus open. */
int strikers_ldn_state(void);                         /* services/nnldn.c: 2..5 = a room is being looked for, hosted or joined */
static volatile LONG g_ff = 1;                        /* the factor in use (1 = normal speed) */
int display_fast_forward(void) { return (int)g_ff; }
static LONG ff_poll(void) {
    static int tick, factor = -1;
    if (factor < 0 || ++tick >= 30) { GfxOptions o; pc_settings_get(&o); factor = o.fast_forward > 1 ? o.fast_forward : 1; tick = 0; }
    /* TEST ONLY, STRIKERS_FF_TEST=<first second>,<seconds>: Tab counts as held for that stretch after the start (no key, any window) */
    static int test_from = -2, test_len; static ULONGLONG test_t0;
    if (test_from == -2) { const char *v = getenv("STRIKERS_FF_TEST"); test_from = -1; test_t0 = GetTickCount64();
        if (v && sscanf(v, "%d,%d", &test_from, &test_len) == 2) printf("display: STRIKERS_FF_TEST (TEST ONLY): fast-forward from %d s for %d s%c", test_from, test_len, 10); else test_from = -1; }
    ULONGLONG since = (GetTickCount64() - test_t0) / 1000;
    int held = test_from >= 0 ? since >= (ULONGLONG)test_from && since < (ULONGLONG)(test_from + test_len)
                              : g_hwnd && GetForegroundWindow() == g_hwnd && (GetAsyncKeyState(VK_TAB) & 0x8000);
    LONG want = 1;
    if (factor > 1 && held && !g_menu_hold && !pc_settings_open()) {
        int st = strikers_ldn_state();
        if (st < 2 || st > 5) want = factor;
    }
    if (want != g_ff) { void os_set_time_scale(double); os_set_time_scale((double)want); InterlockedExchange(&g_ff, want);   /* os/nnsdk.c: the game's own clock */ printf("display: fast-forward %s%c", want > 1 ? (want == 8 ? "8x" : want == 4 ? "4x" : want == 3 ? "3x" : "2x") : "off", 10); }
    return want;
}

int oe_exit_request(void);                            /* nnmisc.c */
static volatile LONG g_closing;
int display_closing(void) { return g_closing != 0; }  /* Removed upstream game feature. */

/* The window's mode (config\strikers_graphics.ini, gfx_options.c): windowed, borderless fullscreen, fullscreen (topmost), or the
 * default, a borderless window of the configured size (dragged anywhere to move it). F11, or a double-click in a borderless
 * window, switches between covering the monitor and the window; a window left by a mode that started covering the monitor
 * is the borderless window of the configured size, centred. Called on the window thread. */
static volatile LONG g_pc_mode;
static HMONITOR g_monitors[32];
static int g_monitor_count;
static BOOL CALLBACK pc_enum_monitor(HMONITOR m,HDC dc,LPRECT rc,LPARAM p) { (void)dc;(void)rc;(void)p;if(g_monitor_count<32)g_monitors[g_monitor_count++]=m;return TRUE; }
static HMONITOR pc_monitor(HWND h) { int n=gfx_options()->monitor;return n>=0 && n<g_monitor_count ? g_monitors[n]:MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST); }
int display_mode(void) {return (int)g_pc_mode;}
void display_set_mode(int mode) {if(g_hwnd)PostMessageA(g_hwnd,WM_APP+16,(WPARAM)mode,0);}
static int g_win_full;                                /* the window covers its monitor now */
static int g_win_drag;                                /* the window is a borderless window now: its whole area moves it */
static int g_win_topmost;                             /* strikers_graphics.ini fullscreen: covering the monitor means on top */
static RECT g_win_rect;                               /* the window's rectangle (and style) before it covered the monitor */
static DWORD g_win_style;
static void window_fullscreen(HWND h, int on) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (on == g_win_full || !GetMonitorInfoA(pc_monitor(h), &mi)) return;
    if (on) {
        GetWindowRect(h, &g_win_rect);
        g_win_style = (DWORD)GetWindowLongA(h, GWL_STYLE);
        SetWindowLongA(h, GWL_STYLE, (LONG)((g_win_style & ~(DWORD)WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE));
        SetWindowPos(h, g_win_topmost ? HWND_TOPMOST : HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        g_win_drag = 0;
    } else {
        if (IsRectEmpty(&g_win_rect)) {
            const GfxOptions *go = gfx_options();
            int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
            int cw = go->width < ww ? go->width : ww, ch = go->height < wh ? go->height : wh;
            SetRect(&g_win_rect, mi.rcWork.left + (ww - cw) / 2, mi.rcWork.top + (wh - ch) / 2, mi.rcWork.left + (ww - cw) / 2 + cw, mi.rcWork.top + (wh - ch) / 2 + ch);
            g_win_style = WS_POPUP | WS_CLIPCHILDREN;
        }
        SetWindowLongA(h, GWL_STYLE, (LONG)(g_win_style | WS_VISIBLE));
        SetWindowPos(h, HWND_NOTOPMOST, g_win_rect.left, g_win_rect.top, g_win_rect.right - g_win_rect.left, g_win_rect.bottom - g_win_rect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        g_win_drag = !(g_win_style & WS_CAPTION);
    }
    g_win_full = on;
    InterlockedExchange(&g_pc_mode,on ? (g_win_topmost?2:1) : (g_win_drag?3:0));
    printf("display: the window %s (F11)\n", on ? "covers its monitor" : g_win_drag ? "is a borderless window" : "has its frame again");
    fflush(stdout);
}

void *display_hwnd(void) { return g_hwnd; }
void display_toggle_fullscreen(void) { if (g_hwnd) window_fullscreen(g_hwnd, !g_win_full); }    /* Removed upstream game feature. */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    /* Audit requests only record when STRIKERS_PC_SHOT enabled readback usage. */
    if(m==WM_APP+17) {pc_capture_request();return 0;}
#ifdef STRIKERS_PC_SCALE_TEST
    if(m==WM_APP+18) {pc_scale_test_request((int)w);return 0;}
#endif
    if(m==WM_APP+16) {
        int mode=(int)w;
        if(g_win_full)window_fullscreen(h,0);
        g_win_topmost=mode==2;
        if(mode==1||mode==2)window_fullscreen(h,1);
        else { DWORD style=(mode==3?WS_POPUP:WS_OVERLAPPEDWINDOW)|WS_CLIPCHILDREN|WS_VISIBLE;SetWindowLongA(h,GWL_STYLE,style);g_win_drag=mode==3;SetWindowPos(h,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);InterlockedExchange(&g_pc_mode,mode); }
        return 0;
    }
    if(m==WM_KEYDOWN && w==VK_F1 && !(l&(1ll<<30))) {pc_settings_toggle();return 0;}
    if((m==WM_KEYDOWN||m==WM_SYSKEYDOWN) && w==VK_F10 && !(l&(1ll<<30))) {void pc_perf_toggle(void);pc_perf_toggle();return 0;}   /* F10: the performance readout */
    if((m==WM_KEYUP||m==WM_SYSKEYUP) && w==VK_F10) return 0;                  /* (F10 alone would open the window menu) */
    if(m==WM_KEYDOWN && w==VK_F9 && !(l&(1ll<<30))) {void bug_report_request(void);bug_report_request();return 0;}   /* F9: a bug report (core/bug_report.c), in every menu too */
    if(m==WM_SYSKEYDOWN && w==VK_RETURN && (l&(1ll<<29)) && !(l&(1ll<<30))) {window_fullscreen(h,!g_win_full);return 0;}
    if(m==WM_DPICHANGED) {RECT *r=(RECT*)l;SetWindowPos(h,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    if(m==WM_TIMER && w==1 && GetForegroundWindow()==h) {POINT pt;RECT rc;if(GetCursorPos(&pt)&&ScreenToClient(h,&pt)&&GetClientRect(h,&rc)&&PtInRect(&rc,pt))SetCursor(pc_cursor_visible()?LoadCursor(NULL,IDC_ARROW):NULL);}
    if(m==WM_SETCURSOR && LOWORD(l)==HTCLIENT) {SetCursor(pc_cursor_visible()?LoadCursor(NULL,IDC_ARROW):NULL);return TRUE;}
    if(m==WM_SIZE || m==WM_MOVE) {if(!g_win_full && !IsIconic(h)) {RECT wr,cr;if(GetWindowRect(h,&wr)&&GetClientRect(h,&cr)&&cr.right>0&&cr.bottom>0)pc_window_geometry(wr.left,wr.top,cr.right,cr.bottom);}}
    if(pc_ui_message(h,m,(unsigned long long)w,(long long)l))return 0;
    if (m == WM_NCHITTEST && g_win_drag && !pc_settings_open()) {           /* a borderless window: dragging anywhere moves it */
        LRESULT r = DefWindowProcA(h, m, w, l);
        return r == HTCLIENT ? HTCAPTION : r;
    }
    if ((m == WM_NCLBUTTONDBLCLK && w == HTCAPTION && g_win_drag) || (m == WM_LBUTTONDBLCLK && g_win_full)) { window_fullscreen(h, !g_win_full); return 0; }
    if (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30))) { window_fullscreen(h, !g_win_full); return 0; }
    if (m == WM_CLOSE) {                             /* the exit request: nn::oe decides (nnmisc.c oe_exit_request) whether the game ends now or after its exit-request handling section */
        GfxOptions saved;pc_settings_get(&saved);saved.window_mode=display_mode();
        if(!g_win_full && !IsIconic(h)) { RECT wr,cr;GetWindowRect(h,&wr);GetClientRect(h,&cr);saved.pos_x=wr.left;saved.pos_y=wr.top;saved.width=cr.right;saved.height=cr.bottom;saved.position_valid=1; }
        gfx_options_save(&saved);
        InterlockedExchange(&g_closing, 1);
        printf("\nwindow closed by the user after %lld frames\n", (long long)g_frames);
        if (!oe_exit_request()) report_and_exit(0);
        return 0;
    }
    if (m == WM_PAINT && !g_frames && splash_paint(h)) return 0;    /* the game's art (build\game-art); below: text only */
    if (m == WM_PAINT && !g_frames) {               /* until the first frame: our own start picture, not a blank window */
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        static ULONGLONG t0; if (!t0) t0 = GetTickCount64();
        int H = rc.bottom > 0 ? rc.bottom : 720, x = H * 96 / 1080;
        HBRUSH ember = CreateSolidBrush(RGB(150, 32, 8)), top = CreateSolidBrush(RGB(31, 2, 10)), bottom = CreateSolidBrush(RGB(66, 8, 20));
        FillRect(dc, &rc, ember);
        RECT bar = rc; bar.bottom = H * 64 / 1080; FillRect(dc, &bar, top);
        bar = rc; bar.top = rc.bottom - H * 64 / 1080; FillRect(dc, &bar, bottom);
        DeleteObject(ember); DeleteObject(top); DeleteObject(bottom);
        SetBkMode(dc, TRANSPARENT);
        HFONT title = CreateFontW(-(H * 96 / 1080), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT sub = CreateFontW(-(H * 30 / 1080), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, title);
        SetTextColor(dc, RGB(255, 255, 255)); TextOutW(dc, x, H * 150 / 1080, L"MARIO STRIKERS", 14);
        SelectObject(dc, sub);
        SetTextColor(dc, RGB(249, 197, 0)); TextOutW(dc, x + 4, H * 260 / 1080, L"B A T T L E   L E A G U E", 25);
        unsigned seconds = (unsigned)((GetTickCount64() - t0) / 1000);
        wchar_t line[96];
        if (seconds < 4) swprintf(line, 96, L"Starting\u2026");
        else swprintf(line, 96, L"Preparing graphics\u2026 %u s (longer the first time, or after a driver update)", seconds);
        SetTextColor(dc, RGB(255, 255, 255)); TextOutW(dc, x, rc.bottom - H * 52 / 1080, line, (int)wcslen(line));
        SelectObject(dc, old); DeleteObject(title); DeleteObject(sub);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER && w == 1 && !g_frames) { static ULONGLONG last; ULONGLONG now = GetTickCount64(); if (now - last >= 1000) { last = now; InvalidateRect(h, NULL, FALSE); } }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_KEYDOWN && w == VK_F12 && !(l & (1 << 30))) {      /* F12 = the console's Capture button (once per press) */
        void strikers_capture_request(void);
        strikers_capture_request();
        return 0;
    }

    return DefWindowProcA(h, m, w, l);
}

static char g_title[96];

void host_thread_name(const char *name);             /* os_threads.c */
static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    host_thread_name("display window");
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnumDisplayMonitors(NULL,NULL,pc_enum_monitor,0);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* Removed upstream game feature. */
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "strikers_native_window";
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wchar_t icon[MAX_PATH];                                   /* the game's icon (build\game-art); none: Windows' default */
    if (game_art_path(icon, MAX_PATH, L"icon.ico")) {
        wc.hIcon = (HICON)LoadImageW(NULL, icon, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
        wc.hIconSm = (HICON)LoadImageW(NULL, icon, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    }
    RegisterClassExA(&wc);
    /* window mode and size from config\strikers_graphics.ini (gfx_options.c). borderless window (the default): frameless,
     * the configured size (at most the primary monitor's work area), centred on it. borderless fullscreen / fullscreen: a
     * frameless window over the primary monitor (fullscreen: also topmost). F11 switches (window_fullscreen). */
    const GfxOptions *go = gfx_options();
    DWORD style = (go->window_mode ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN;      /* Removed upstream game feature. */
    int cw = go->width, ch = go->height, px = CW_USEDEFAULT, py = CW_USEDEFAULT;
    if (go->window_mode) {
        MONITORINFO mi;
        POINT origin = {0, 0};
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(go->monitor>=0 && go->monitor<g_monitor_count ? g_monitors[go->monitor] : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            if (go->window_mode == 3) {
                int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
                if (cw > ww) cw = ww;
                if (ch > wh) ch = wh;
                px = mi.rcWork.left + (ww - cw) / 2; py = mi.rcWork.top + (wh - ch) / 2;
            } else { cw = mi.rcMonitor.right - mi.rcMonitor.left; ch = mi.rcMonitor.bottom - mi.rcMonitor.top; px = mi.rcMonitor.left; py = mi.rcMonitor.top; }
        }
    }
    g_win_full = go->window_mode == 1 || go->window_mode == 2;
    g_win_drag = go->window_mode == 3;
    g_win_topmost = go->window_mode == 2;
    InterlockedExchange(&g_pc_mode,go->window_mode);
    if(!g_win_full && go->position_valid) { RECT saved={go->pos_x,go->pos_y,go->pos_x+cw,go->pos_y+ch}; if(MonitorFromRect(&saved,MONITOR_DEFAULTTONULL)) {px=go->pos_x;py=go->pos_y;} }
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, style, FALSE);
    printf("display: window %s, client %dx%d (strikers_graphics.ini; F11 switches between covering the monitor and the window)\n",
           go->window_mode == 2 ? "fullscreen (borderless, topmost)" : go->window_mode == 1 ? "borderless fullscreen" : go->window_mode == 3 ? "borderless window" : "windowed", cw, ch);
    /* Only the copies a person plays (strikers_native.exe, strikers_user.exe, or STRIKERS_SHOW=1) get a window on screen;
     * test copies started under any other name keep theirs off-screen and out of the taskbar, so automated
     * runs do not cover the desktop. STRIKERS_TITLE names the window. */
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, *show = getenv("STRIKERS_SHOW");
    int visible = show ? atoi(show) != 0 : (!_stricmp(base, "strikers_native.exe") || !_stricmp(base, "strikers_user.exe"));
    pc_start_screen_maybe(visible);
    snprintf(g_title, sizeof g_title, "%s", getenv("STRIKERS_TITLE") ? getenv("STRIKERS_TITLE") : "Strikers Recomp");
    if (visible)
        g_hwnd = CreateWindowExA(go->window_mode == 2 ? WS_EX_TOPMOST : 0, wc.lpszClassName, g_title, style | WS_VISIBLE, px, py, r.right - r.left,
                                 r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    else {
        g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, g_title, style, -32000, -32000,
                                 r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    SetTimer(g_hwnd,1,100,NULL);
    InterlockedExchange(&g_ready, 1);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

/* ---- the monitor's own refresh (audit 02). The refresh clock below is QueryPerformanceCounter's 60 Hz; the monitor has a
 * clock of its own (this PC: 59.997 Hz). A frame reaches the screen at the monitor's first refresh after its present, so
 * while the presents fall close to a monitor refresh they land on either side of it by chance: two frames in one refresh (the
 * first is never seen) next to a refresh with none (the frame before is shown twice). dvb_thread waits for the monitor's
 * refreshes (D3DKMTWaitForVerticalBlankEvent on the window's monitor); display_presented counts, per present, how many
 * monitor refreshes the frame stayed on screen against the refreshes the game asked for ("display_vblank:" lines).
 * STRIKERS_VSYNC_LOCK (default 1 where the monitor runs at 60 Hz or a whole multiple of it within 0.2%; 0 = off): the
 * refresh clock ticks at the monitor's refreshes (every k-th at k x 60 Hz) instead of its own, so the game's steps and the
 * monitor stay in phase. Without a monitor refresh for 50 ms (monitor asleep, remote session) a tick comes from the clock.
 * STRIKERS_VBLANKSTAT=0: no monitor refresh thread at all (no lock, no statistics). */
typedef UINT KmtHandle;
typedef struct { HDC hdc; KmtHandle adapter; LUID luid; UINT source; } KmtOpenHdc;
typedef struct { KmtHandle adapter; KmtHandle device; UINT source; } KmtWaitVblank;
static LONG (APIENTRY *p_kmt_wait)(const KmtWaitVblank *);
static KmtWaitVblank g_kmt;
static HMONITOR g_dvb_monitor;
static LONG (APIENTRY *p_kmt_close)(const KmtHandle *);
static SRWLOCK g_dvb_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_dvb_cv = CONDITION_VARIABLE_INIT;
static volatile LONG64 g_dvb_n, g_dvb_t, g_dvb_period;   /* monitor refreshes seen, QPC of the last, QPC ticks per refresh (0: not known yet) */
static volatile LONG g_dvb_k;                            /* > 0: the refresh clock follows every k-th monitor refresh */
static volatile LONG64 g_dvb_missed;                     /* monitor refreshes the thread woke up too late for */
static double g_dvb_hz;
static int dvb_open(void) {
    LONG (APIENTRY *open)(KmtOpenHdc *);
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) gdi = LoadLibraryA("gdi32.dll");
    if (!gdi) return 0;
    *(FARPROC *)&open = GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
    *(FARPROC *)&p_kmt_wait = GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
    *(FARPROC *)&p_kmt_close = GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!open || !p_kmt_wait) return 0;
    MONITORINFOEXA mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    HMONITOR monitor=MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoA(monitor, (MONITORINFO *)&mi)) return 0;
    KmtOpenHdc o; memset(&o, 0, sizeof o);
    if (!(o.hdc = CreateDCA(NULL, mi.szDevice, NULL, NULL))) return 0;
    LONG r = open(&o);
    DeleteDC(o.hdc);
    if (r) return 0;
    if(g_kmt.adapter && p_kmt_close)p_kmt_close(&g_kmt.adapter);
    g_kmt.adapter = o.adapter; g_kmt.source = o.source;g_dvb_monitor=monitor;
    return 1;
}
static DWORD WINAPI dvb_thread(LPVOID arg) {
    (void)arg;
    static LONG64 ring[128], ring_n[128];  /* audit 05: with the refresh number of each wake (a wake can come after a missed refresh) */
    LONG64 seen = 0, fails = 0;
    host_thread_name("display monitor refreshes");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        if(MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTOPRIMARY)!=g_dvb_monitor) {
            InterlockedExchange(&g_dvb_k,0);
            if(dvb_open()) {seen=0;memset(ring,0,sizeof ring);memset(ring_n,0,sizeof ring_n);InterlockedExchange64(&g_dvb_period,0);printf("display: monitor changed; measuring the new monitor's refresh clock\n");}
        }
        if (p_kmt_wait(&g_kmt)) {
            if (++fails == 50) { printf("display: the monitor's refreshes cannot be waited for: the refresh clock keeps its own 60 Hz%c", 10); InterlockedExchange(&g_dvb_k, 0); WakeAllConditionVariable(&g_dvb_cv); return 0; }
            Sleep(10); continue;
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        LONG64 n = g_dvb_n + 1, per = g_dvb_period;
        if (per && now.QuadPart - g_dvb_t > per + per / 2) { LONG64 d = (now.QuadPart - g_dvb_t + per / 2) / per; n = g_dvb_n + d; InterlockedExchangeAdd64(&g_dvb_missed, d - 1); }
        ring_n[seen & 127] = n; ring[seen++ & 127] = now.QuadPart;
        int decide = 0;
        if (seen > 128 && seen % 16 == 0) {      /* the period over the last 127 waits, taken when two such spans agree (a late wake, a new refresh rate) */
            static LONG64 last_span;
            /* audit 05: per refresh, not per wake. Two spans of the last 127 wakes overlap in 111 of them, so one wake that came a
             * refresh late counted in both, they agreed, and a period 0.8% long was taken (236 Hz on this PC's 240 Hz monitor:
             * the clock left the monitor until the next measure, 3-7 times a match). Missed refreshes are counted (n) once the
             * period is known. The comparison is now of the periods. */
            LONG64 refreshes = ring_n[(seen - 1) & 127] - ring_n[seen & 127];
            LONG64 span = refreshes > 0 ? (ring[(seen - 1) & 127] - ring[seen & 127]) / refreshes * 127 : 0;
            if (last_span && span && span > last_span - last_span / 200 && span < last_span + last_span / 200) { InterlockedExchange64(&g_dvb_period, span / 127); decide = seen >= 144; }
            last_span = span;
        }
        AcquireSRWLockExclusive(&g_dvb_srw);
        g_dvb_t = now.QuadPart; InterlockedExchange64(&g_dvb_n, n);
        ReleaseSRWLockExclusive(&g_dvb_srw);
        WakeAllConditionVariable(&g_dvb_cv);
        if (decide) {                             /* from about 2 s in, at every new measure: may the clock follow the monitor (again, still)? */
            static int said = -1;
            const char *e = getenv("STRIKERS_VSYNC_LOCK");
            double hz = (double)g_freq.QuadPart / (double)g_dvb_period;
            int k = (int)(hz / 60.0 + 0.5), want = !(e && !atoi(e)), fit = k >= 1 && k <= 4 && hz > 60.0 * k * 0.998 && hz < 60.0 * k * 1.002 && g_rate == 60.0;
            int now_k = want && fit ? k : 0, state = now_k * 4 + fit * 2 + want;
            g_dvb_hz = hz;
            if (g_dvb_k != now_k) InterlockedExchange(&g_dvb_k, now_k);
            if (state != said) {
                said = state;
                printf("display: the monitor refreshes at %.3f Hz; the refresh clock %s%c", hz,
                       now_k ? (k == 1 ? "follows it (STRIKERS_VSYNC_LOCK=0: its own 60 Hz)" : "follows every k-th refresh (STRIKERS_VSYNC_LOCK=0: its own 60 Hz)") :
                       fit ? "keeps its own 60 Hz (STRIKERS_VSYNC_LOCK=0)" : "keeps its own 60 Hz (not 60 Hz or a multiple within 0.2%)", 10);
            }
        }
    }
}
/* nvn_vk_draw.c, right after a frame's vkQueuePresentKHR (render thread): the monitor refresh it will be shown at, against the
 * refresh the game's clock gives it (frame_queued's rule). */
static LONG64 g_pv_n, g_pv_err, g_pv_dropped, g_pv_hist[4], g_pv_near;
static volatile LONG g_interval;
void display_presented(void) {
    LONG64 per = g_dvb_period;
    if (!per || old_pacing()) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    AcquireSRWLockShared(&g_dvb_srw);
    LONG64 n = g_dvb_n, t = g_dvb_t;
    ReleaseSRWLockShared(&g_dvb_srw);
    LONG64 ph = now.QuadPart - t;
    if (ph < 0) ph = 0;
    n += ph / per; ph %= per;                      /* the refresh thread has not woken yet for a refresh that passed */
    static LONG64 last_idx = -1, last_soft = -1;
    static LONG64 w_n, w_err, w_dropped, w_hist[4], w_near;
    LONG64 idx = n + 1, iv = g_interval, soft = g_vsync_n + 1;
    if (iv && last_soft >= 0 && soft < last_soft + iv) soft = last_soft + iv;
    if (last_idx >= 0) {
        LONG64 d = idx - last_idx, ds = soft - last_soft, k = g_dvb_k ? g_dvb_k : 1;
        int b = d <= 0 ? 0 : d >= 3 ? 3 : (int)d;
        g_pv_hist[b]++; w_hist[b]++; g_pv_n++; w_n++;
        if (d <= 0) { g_pv_dropped++; w_dropped++; }
        if (d != ds * k) { g_pv_err++; w_err++; }
        if (per - ph < g_freq.QuadPart / 1000) { g_pv_near++; w_near++; }
    }
    last_idx = idx; last_soft = soft;
    if (w_n >= 1800) {                             /* render thread: the line goes to nvn_vk_draw.c's log thread (stdout is unbuffered) */
        int nvn_log_async(char *text);
        char line[512];
        snprintf(line, sizeof line, "display_vblank: %lld presents: monitor refreshes per frame 0:%lld 1:%lld 2:%lld 3+:%lld | frames never shown %lld, shown for other than the game's refreshes %lld | presents in the last 1 ms before a refresh %lld | monitor %.3f Hz, clock %s, refreshes the wait thread woke late for %lld%c",
               (long long)w_n, (long long)w_hist[0], (long long)w_hist[1], (long long)w_hist[2], (long long)w_hist[3], (long long)w_dropped, (long long)w_err, (long long)w_near, g_dvb_hz, g_dvb_k ? "follows the monitor" : "own 60 Hz", (long long)g_dvb_missed, 10);
        char *copy = _strdup(line);
        if (!copy || !nvn_log_async(copy)) { fputs(line, stdout); free(copy); }
        w_n = w_err = w_dropped = w_near = 0; memset(w_hist, 0, sizeof w_hist);
    }
}

/* Signals the guest's vsync event 60 times per second. */
static DWORD WINAPI vsync_thread(LPVOID arg) {
    (void)arg;
    LARGE_INTEGER next;
    host_thread_name("display refresh clock");
    QueryPerformanceCounter(&next);
    if (!old_pacing()) {
        LARGE_INTEGER t0 = next, now;
        LONG64 n = 0;
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        LONG64 dvb_last = -1;
        int dvb_rest = 0;                          /* ticks from the clock after the monitor stopped answering, before it is waited for again */
        double rate = g_rate;
        for (;;) {
            LONG ff = ff_poll();
            if (rate != g_rate * ff) {             /* fast-forward began or ended: the same count goes on at the new rate from now */
                rate = g_rate * ff;
                QueryPerformanceCounter(&now);
                t0.QuadPart = now.QuadPart - (LONGLONG)((double)n * (double)g_freq.QuadPart / rate);
                dvb_last = -1;
            }
            LONG k = ff > 1 ? 0 : g_dvb_k;         /* fast: the clock's own ticks, not the monitor's */
            if (dvb_rest > 0) { dvb_rest--; k = 0; dvb_last = -1; }
            if (k > 0) {                           /* STRIKERS_VSYNC_LOCK: the k-th monitor refresh after the last tick */
                LONG64 got;
                AcquireSRWLockExclusive(&g_dvb_srw);
                if (dvb_last < 0) dvb_last = g_dvb_n;
                while (g_dvb_n < dvb_last + k && g_dvb_k) if (!SleepConditionVariableSRW(&g_dvb_cv, &g_dvb_srw, 50, 0)) break;
                got = g_dvb_n;
                ReleaseSRWLockExclusive(&g_dvb_srw);
                QueryPerformanceCounter(&now);
                if (got >= dvb_last + k) {
                    LONG64 steps = (got - dvb_last) / k;
                    dvb_last += steps * k;
                    if (steps > 1) InterlockedExchangeAdd64(&g_vsync_late, steps - 1);
                    n += steps;
                    t0.QuadPart = now.QuadPart - (LONGLONG)((double)n * (double)g_freq.QuadPart / g_rate);      /* the clock's own ticks go on from here if the monitor stops */
                    goto signal;
                }
                dvb_last = -1; dvb_rest = 120;     /* no monitor refresh for 50 ms (asleep, remote session): 2 s of ticks from the clock */
            }
            LONGLONG target = t0.QuadPart + (LONGLONG)((double)(n + 1) * (double)g_freq.QuadPart / rate);      /* no accumulated rounding */
            QueryPerformanceCounter(&now);
            LONGLONG left = target - now.QuadPart;
            if (left > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);
                if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
                for (QueryPerformanceCounter(&now); now.QuadPart < target; QueryPerformanceCounter(&now)) YieldProcessor();
            }
            LONG64 passed = (LONG64)((double)(now.QuadPart - t0.QuadPart) * rate / (double)g_freq.QuadPart);   /* >= n + 1; more when this thread was kept off the CPU */
            if (passed < n + 1) passed = n + 1;
            if (passed > n + 1) InterlockedExchangeAdd64(&g_vsync_late, passed - (n + 1));
            n = passed;
        signal:
            if (g_vsync_event && vsync_signal_ok()) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
            AcquireSRWLockExclusive(&g_vsync_lock);
            InterlockedExchange64(&g_vsync_n, n);
            ReleaseSRWLockExclusive(&g_vsync_lock);
            WakeAllConditionVariable(&g_vsync_cv);
            nvn_tl(8 /* TL_VSYNC */, (int)(n & 0xffff));
            vsync_test_key();
        }
    }
    for (;;) {
        next.QuadPart += g_freq.QuadPart / 60;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 0) Sleep((DWORD)ms); else next = now;
        if (g_vsync_event && vsync_signal_ok()) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
        vsync_test_key();
    }
}
static void vsync_test_key(void) {
    {
        /* Test aid: STRIKERS_AUTOKEY=<letter>,<first second>,<every n seconds> presses a key now and then, so a run without a person can get
         * past "press any button". Only while this window is the foreground window, so the key never reaches another program. */
        static int key = -1, first, every, tick, down;
        if (key == -1) {
            const char *e = getenv("STRIKERS_AUTOKEY");
            key = 0;
            if (e && e[0]) { key = e[0]; first = 30; every = 5; sscanf(e + 1, ",%d,%d", &first, &every); if (every < 1) every = 1; }
        }
        tick++;
        if (key && down && tick - down >= 8) { keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0); down = 0; }
        else if (key && !down && tick >= first * 60 && (tick - first * 60) % (every * 60) == 0 && ((tick % 600 == 0 ? (void)printf("display: test key wanted, window foreground: %d%c", GetForegroundWindow() == g_hwnd, 10) : (void)0), 1) && (GetForegroundWindow() == g_hwnd || (SetForegroundWindow(g_hwnd), GetForegroundWindow() == g_hwnd))) { keybd_event((BYTE)key, 0, 0, 0); down = tick; printf("display: test key %c pressed at frame tick %d%c", key, tick, 10); }
    }
}

void hitch_init(void); void hitch_pacing(int on);      /* hitch.c: STRIKERS_HITCH */
void display_init(void) {
    /* Per-process awareness keeps GetClientRect on the render thread in the
     * same physical pixel space as the window and Vulkan surface. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_freq);
    /* the console's waits with a time-out (nn::os::TimedWaitEvent, TimedReceiveMessageQueue, timed condition
     * variables: os_threads.c, SleepConditionVariableCS with whole milliseconds) end at the time asked for. At the
     * default Windows timer period they ended at the next 15.6 ms tick, so a thread that polls with a 1 ms time-out ran
     * 64 times per second: the movie player's two feed threads delivered one sample per tick and its pre-roll loop
     * [game 0x71036f27b4, 125 x 10 ms, holding the player's mutex] ran out every time (1.3 s, main thread blocked).
     * STRIKERS_TIMER_PERIOD=<ms> (default 1; 0 = leave the system default). */
    { const char *e = getenv("STRIKERS_TIMER_PERIOD"); int ms = e ? atoi(e) : 1; if (ms > 0) { MMRESULT r = timeBeginPeriod((UINT)ms); printf("display: system timer period %d ms requested (%s)%c", ms, r == TIMERR_NOERROR ? "ok" : "refused", 10); } }
    hitch_init();
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    while (!g_ready) Sleep(1);
    nvn_gl_start(g_hwnd);
    speed_init();
    if (!old_pacing() && !(getenv("STRIKERS_VBLANKSTAT") && !atoi(getenv("STRIKERS_VBLANKSTAT")))) {
        if (dvb_open()) CreateThread(NULL, 0, dvb_thread, NULL, 0, NULL);
        else printf("display: the window's monitor cannot be opened for its refreshes (D3DKMT): the refresh clock keeps its own 60 Hz%c", 10);
    }
    CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
    { void nvn_vk_selftest_hook(void); nvn_vk_selftest_hook(); }      /* nvn_vk.c: STRIKERS_RENDERER=vk STRIKERS_VK_SELFTEST=<frames>, then exit */
}

/* Called after every presented frame (nvn_gl.c has already put it in the window): holds the caller to 60 Hz. */
static volatile LONG g_interval = 1;         /* nvnWindowSetPresentInterval: display refreshes per presented frame (0 = do not wait) */
void display_set_interval(int n) { InterlockedExchange(&g_interval, n < 0 ? 0 : n > 5 ? 5 : n); }
void nvn_pf_pace(LONG64 ticks);            /* nvn_gl.c: timing scopes */
/* STRIKERS_CLOCKSTAT=1: measure the host refresh signal against QPC. The
 * inherited Odyssey window-object offsets are not a Strikers clock contract;
 * reading them produced meaningless (sometimes negative) "game steps". */
void display_clock_sample(uint64_t index_ptr) {
    (void)index_ptr;
}
static void clock_stat(LONG64 n, LARGE_INTEGER now) {
    static LARGE_INTEGER t_last; static LONG64 sig_last, late_last;
    if (!g_clockstat || n % 600) return;
    LONG64 sig = g_vsync_signals, late = g_vsync_late;
    if (t_last.QuadPart) printf("clock: frames %lld-%lld real %.3f s, vsync signals %lld (%.2f Hz), slept through by the vsync thread %lld%c", (long long)(n - 600), (long long)n,
        (double)(now.QuadPart - t_last.QuadPart) / (double)g_freq.QuadPart, (long long)(sig - sig_last), (double)(sig - sig_last) * (double)g_freq.QuadPart / (double)(now.QuadPart - t_last.QuadPart), (long long)(late - late_last), 10);
    t_last = now; sig_last = sig; late_last = late;
}
/* New pacing, part 1 (nvnQueuePresentTexture, after the frame is in the window): decides the refresh that shows it. */
static void frame_queued(void) {
    static LARGE_INTEGER second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!second_start.QuadPart) second_start = now;
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    LONG64 at = g_vsync_n + 1, iv = g_interval;
    if (!iv) at = g_vsync_n;                                       /* interval 0: shown at once, nobody waits */
    else if (at < g_latch + iv) at = g_latch + iv;
    g_latch = at;
    InterlockedExchange(&g_latch_pending, iv != 0);
    clock_stat(n, now);
    {   /* TEST ONLY, STRIKERS_FRAMELOG=<k>: a line "frame-mark: <n>" every k presented frames, to place log events in game frames */
        static int every = -1;
        if (every < 0) { const char *e = getenv("STRIKERS_FRAMELOG"); every = e ? atoi(e) : 0; }
        if (every > 0 && n % every == 0) printf("frame-mark: %lld%c", (long long)n, 10);
    }
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) {       /* frame_time_overlay = on */
            double ms = (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames;
            printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", ms, (long long)second_frames);
            snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, ms);
        } else
        snprintf(title, sizeof title, "%s", g_title);           /* just the game's name, as a PC game's window has */
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}
/* New pacing, part 2: holds the caller until the refresh that shows the last queued frame. Returns 1 if it waited. */
int display_wait_refresh(void) {
    if (old_pacing() || !InterlockedExchange(&g_latch_pending, 0)) return 0;
    if (g_vsync_n >= g_latch) return 0;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    hitch_pacing(1);
    nvn_tl(6 /* TL_PACE_BEGIN */, (int)(g_latch & 0xffff));
    AcquireSRWLockExclusive(&g_vsync_lock);
    while (g_vsync_n < g_latch) SleepConditionVariableSRW(&g_vsync_cv, &g_vsync_lock, 100, 0);
    ReleaseSRWLockExclusive(&g_vsync_lock);
    nvn_tl(7 /* TL_PACE_END */, (int)(g_vsync_n & 0xffff));
    hitch_pacing(0);
    QueryPerformanceCounter(&b);
    nvn_pf_pace(b.QuadPart - a.QuadPart);
    return 1;
}
/* Removed upstream game feature. */
void display_menu_wait(void) {
    if (old_pacing()) { Sleep(16); return; }
    LONG64 n = g_vsync_n;
    AcquireSRWLockExclusive(&g_vsync_lock);
    while (g_vsync_n == n) if (!SleepConditionVariableSRW(&g_vsync_cv, &g_vsync_lock, 100, 0)) break;
    ReleaseSRWLockExclusive(&g_vsync_lock);
}
/* Before a present: a frame queued earlier that nobody waited for (a caller that does not wait on the window sync). */
void display_before_present(void) {
    static int said;
    if (old_pacing() || !g_latch_pending) return;
    if (!said) { said = 1; printf("display: a present without a wait on the window texture's sync: held at the next present instead%c", 10); }
    display_wait_refresh();
}
void display_pace(void) {
    static LARGE_INTEGER next, second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    if (!old_pacing()) { frame_queued(); return; }
    QueryPerformanceCounter(&now);
    if (!next.QuadPart) { next = now; second_start = now; }
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    clock_stat(n, now);
    next.QuadPart += g_freq.QuadPart * g_interval / 60;
    /* wait until `next` exactly. Sleep(ms) cut the wait to whole milliseconds and, at the default 15.6 ms timer period,
     * could overshoot by a frame; a frame that came late was then followed by frames shown early (up to 100 ms of debt).
     * Now: a high-resolution waitable timer up to 0.5 ms before the deadline, then a spin; a late frame may be made up for
     * by at most one refresh interval, as a display that missed a refresh shows the next frame at the next one. */
    LONGLONG left = next.QuadPart - now.QuadPart, frame_ticks = g_freq.QuadPart * (g_interval ? g_interval : 1) / 60;
    if (!g_interval) next = now;
    else if (left > 0) {
        static HANDLE timer;
        LARGE_INTEGER after, due;
        hitch_pacing(1);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);          /* relative, 100 ns units */
        if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
        for (QueryPerformanceCounter(&after); after.QuadPart < next.QuadPart; QueryPerformanceCounter(&after)) YieldProcessor();
        hitch_pacing(0);
        nvn_pf_pace(after.QuadPart - now.QuadPart);
    } else if (-left > frame_ticks) next.QuadPart = now.QuadPart - frame_ticks;
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames, (long long)second_frames);
        if (gfx_options()->overlay && second_frames) snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames);
        else
        snprintf(title, sizeof title, "%s", g_title);           /* just the game's name, as a PC game's window has */
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}

long long display_frames(void) { return g_frames; }  /* Removed upstream game feature. */

void display_report(FILE *out) {
    fprintf(out, "\ndisplay: %lld frames presented\n", (long long)g_frames);
    fprintf(out, "display: %s; %lld vsync signals, %lld refreshes slept through by the vsync thread\n", old_pacing() ? "old pacing (STRIKERS_OLD_PACING=1: two clocks)" : "one refresh clock", (long long)g_vsync_signals, (long long)g_vsync_late);
    if (g_pv_n) fprintf(out, "display_vblank_total: %lld presents: monitor refreshes per frame 0:%lld 1:%lld 2:%lld 3+:%lld | frames never shown %lld, shown for other than the game's refreshes %lld | presents in the last 1 ms before a refresh %lld | monitor %.3f Hz, clock %s, refreshes the wait thread woke late for %lld\n",
                        (long long)g_pv_n, (long long)g_pv_hist[0], (long long)g_pv_hist[1], (long long)g_pv_hist[2], (long long)g_pv_hist[3], (long long)g_pv_dropped, (long long)g_pv_err, (long long)g_pv_near, g_dvb_hz, g_dvb_k ? "follows the monitor" : "own 60 Hz", (long long)g_dvb_missed);
    nvn_gl_report(out);
}

/* ---- nn::vi: one display, one layer; the native window is an opaque token for NVN */
static uint8_t g_display_token[16], g_layer_token[16], g_native_window_token[16];
static void vi_OpenDefaultDisplay(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_display_token); RET(0); }
static void vi_CreateLayer(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_layer_token); RET(0); }
static void vi_GetNativeWindow(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_native_window_token); RET(0); }
static void vi_GetDisplayVsyncEvent(Ctx *c) {         /* (SystemEventType *out, Display *) */
    os_init_event(A0, 1);
    g_vsync_event = A0;
    RET(0);
}
static void nothing(Ctx *c) { (void)c; }

static const ImportDef vi_imports[] = {
    {"_ZN2nn2vi10InitializeEv", nothing},
    {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay},
    {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer},
    {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_GetDisplayVsyncEvent},
};

void register_display_imports(void) {
    register_imports(vi_imports, (int)(sizeof vi_imports / sizeof vi_imports[0]));
}
