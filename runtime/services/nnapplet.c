/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: strikers-recomp/runtime/services/nnapplet.c (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);
void os_clear_event(uint64_t key);

/* ================================================================ nn::web */
#define WEB_RESULT_TERMINATED 0x2d2u
#define WEB_EXIT_END_BUTTON 0u
#define WEB_EXIT_CALLBACK_URL 3u
static const char g_last_url[] = "http://localhost/";

/* Input TLVs of a Show*Arg ([doc] switchbrew "Internet Browser": header u16 count, u16 pad, u32 shim kind; entries
 * u16 type, u16 size, u32 pad; type 1 initial URL, 6 document path). Only used to say in the log what was asked. */
static const char *web_arg_string(uint64_t arg, int type) {
    uint32_t count = rd16(arg), off = 8;
    for (uint32_t i = 0; i < count && off + 8 < 0x2000; i++) {
        uint32_t t = rd16(arg + off), size = rd16(arg + off + 2);
        if ((int)t == type) return (const char *)(uintptr_t)(arg + off + 8);
        off += size + 8;
    }
    return NULL;
}
static void web_log(Ctx *c, const char *what, uint64_t arg) {
    const char *url = web_arg_string(arg, 1), *doc = web_arg_string(arg, 6);
    printf("%s: \"%.200s\" cannot be shown here; answered as closed by the user (caller 0x%llx)\n", what, doc ? doc : url ? url : "",
           (unsigned long long)c->x[30] - 4);
    trace_printf("     %s \"%.200s\" -> closed (EndButtonPressed)\n", what, doc ? doc : url ? url : "");
}
static void web_fill_return(uint64_t ret, uint32_t reason, const char *url) {      /* 0x1010 bytes */
    size_t n = strlen(url) < 0xfff ? strlen(url) : 0xfff;
    memset((void *)(uintptr_t)ret, 0, 0x1010);
    wr32(ret, reason);
    memcpy((void *)(uintptr_t)(ret + 8), url, n);
    wr64(ret + 0x1008, n);
}
/* Offline web policy: closed by user. Hosting a browser is a later module. */
static void web_ShowOfflineHtmlPage(Ctx *c) {
    web_log(c, "nn::web::ShowOfflineHtmlPage", A1);
    web_fill_return(A0, WEB_EXIT_END_BUTTON, g_last_url); RET(0);
}

/* ================================================================ nn::oe volume balance
 * [sdk] 0x32c5b0 SetExpectedVolumeBalance(float mainApplet, float libraryApplet): one request to the applet
 * manager's audio controller (object from 0x5368a0, slot +0x20 = command 0 SetExpectedMasterVolume); a failure
 * aborts. [sdk] 0x32c620 GetExpectedVolumeBalance(float *mainApplet, float *libraryApplet): two requests
 * (slot +0x28 = command 1 GetMainAppletExpectedMasterVolume, slot +0x30 = command 2
 * GetLibraryAppletExpectedMasterVolume), each failure aborts. Nothing else happens on the SDK side.
 * The applet manager's side (not in the dump), after [doc] suyu am/service/audio_controller.cpp/.h: it keeps
 * the two values clamped to [0, 1]; initial values main 0.25, library 1.0; the reference does nothing else
 * with them. What the console's applet manager does to the SOUND with them while a library applet is in front
 * is not in any source available here: NOT implemented, NOT verified. [game] 0x71037552fc saves them around
 * the offline HTML page and restores them afterwards. */
static float g_volume_main = 0.25f, g_volume_library = 1.0f;
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static void oe_GetExpectedVolumeBalance(Ctx *c) {
    memcpy((void *)(uintptr_t)A0, &g_volume_main, 4);
    memcpy((void *)(uintptr_t)A1, &g_volume_library, 4);
}
static void oe_SetExpectedVolumeBalance(Ctx *c) { g_volume_main = clamp01(c->v[0].f[0]); g_volume_library = clamp01(c->v[1].f[0]); }

/* ================================================================ nn::swkbd::ShowKeyboard */
#define SWKBD_RESULT_CANCELED 0x29fu
#define KC_HEADER 0x24
#define KC_SUB 0xa6
#define KC_GUIDE 0x1a8
#define KC_MAXLEN 0x3ac
#define KC_MINLEN 0x3b0
#define KC_PASSWORD 0x3b4
#define KC_UTF8 0x3bd
#define KC_INITIAL_OFFSET 0x3c0
#define KC_INITIAL_LENGTH 0x3c4
#define KC_CHECK_ENABLED 0x3d0
#define KC_CHECK_CALLBACK 0x3d8
#define ARG_WORKBUF 0x4d0
#define ARG_CHECKBUF 0x4e0
#define SWKBD_MAX_CHARS 500                               /* [sdk] 0x38b150: SetInitialText stops at 0x1f4 characters */

typedef struct {
    const wchar_t *header, *sub, *guide, *initial;
    int max_len, min_len, password;
    wchar_t text[SWKBD_MAX_CHARS + 2];
} KbdDialog;

#define IDC_EDIT 1001
#define IDC_INFO 1002
static INT_PTR CALLBACK kbd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    KbdDialog *k = (KbdDialog *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        k = (KbdDialog *)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)k);
        wchar_t info[600];
        _snwprintf(info, 599, L"%ls%ls%ls%ls%ls", k->header, k->header[0] && k->sub[0] ? L"\r\n" : L"", k->sub,
                   (k->header[0] || k->sub[0]) && k->guide[0] ? L"\r\n" : L"", k->guide);
        info[599] = 0;
        SetDlgItemTextW(dlg, IDC_INFO, info[0] ? info : L"Enter text");
        SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETLIMITTEXT, (WPARAM)k->max_len, 0);
        if (k->password) SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
        SetDlgItemTextW(dlg, IDC_EDIT, k->initial);
        SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETSEL, 0, -1);
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_EDIT, k->text, SWKBD_MAX_CHARS + 1);
            if ((int)wcslen(k->text) < k->min_len) { MessageBeep(MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    case WM_CLOSE: EndDialog(dlg, 0); return TRUE;
    }
    return FALSE;
}
static uint8_t *tmpl_align(uint8_t *p, int a) { while ((uintptr_t)p & (uintptr_t)(a - 1)) *p++ = 0; return p; }
static uint8_t *tmpl_wstr(uint8_t *p, const wchar_t *s) { size_t n = (wcslen(s) + 1) * sizeof(wchar_t); memcpy(p, s, n); return p + n; }
static uint8_t *tmpl_item(uint8_t *p, DWORD style, int x, int y, int w, int h, int id, WORD cls, const wchar_t *text) {
    p = tmpl_align(p, 4);
    DLGITEMTEMPLATE it = {style | WS_CHILD | WS_VISIBLE, 0, (short)x, (short)y, (short)w, (short)h, (WORD)id};
    memcpy(p, &it, sizeof it); p += sizeof it;
    WORD cl[2] = {0xffff, cls};
    memcpy(p, cl, 4); p += 4;
    p = tmpl_wstr(p, text);
    *p++ = 0; *p++ = 0;                                   /* no creation data */
    return p;
}
static int kbd_run_dialog(KbdDialog *k) {
    uint8_t buf[1024], *p = buf;
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE dt = {DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFOREGROUND, WS_EX_TOPMOST, 4, 0, 0, 260, 96};
    memcpy(p, &dt, sizeof dt); p += sizeof dt;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;               /* no menu, default class */
    p = tmpl_wstr(p, L"Software keyboard (strikers-recomp)");
    *p++ = 9; *p++ = 0;
    p = tmpl_wstr(p, L"Segoe UI");
    p = tmpl_item(p, SS_LEFT, 8, 6, 244, 34, IDC_INFO, 0x0082, L"");
    p = tmpl_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 44, 244, 14, IDC_EDIT, 0x0081, L"");
    p = tmpl_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 144, 72, 50, 16, IDOK, 0x0080, L"OK");
    p = tmpl_item(p, BS_PUSHBUTTON | WS_TABSTOP, 202, 72, 50, 16, IDCANCEL, 0x0080, L"Cancel");
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (DLGTEMPLATE *)buf, NULL, kbd_proc, (LPARAM)k);
    if (r == -1) { printf("nn::swkbd: the text dialog could not be created (error %lu); answered as cancelled\n", GetLastError()); return 0; }
    return (int)r;
}
static void copy_w(wchar_t *dst, uint64_t src, int max) {
    int i = 0;
    for (; i < max; i++) { uint16_t ch = rd16(src + (uint64_t)i * 2); if (!ch) break; dst[i] = ch; }
    dst[i] = 0;
}
/* How this call is answered: 0 dialog, 1 cancel, 2 fixed text. */
static int kbd_mode(wchar_t *fixed) {
    const char *v = getenv("STRIKERS_SWKBD");
    if (v && strncmp(v, "text:", 5) == 0) { MultiByteToWideChar(CP_UTF8, 0, v + 5, -1, fixed, SWKBD_MAX_CHARS + 1); fixed[SWKBD_MAX_CHARS] = 0; return 2; }
    if (v && strcmp(v, "cancel") == 0) return 1;
    if (v && strcmp(v, "dialog") == 0) return 0;
    if (getenv("STRIKERS_SEQ") || getenv("STRIKERS_PRESS") || getenv("STRIKERS_SCRIPT")) return 1;
    return 0;
}
/* Writes `text` into the caller's String the way the applet's storage is copied ([sdk] 0x389ff4):
 * at most bufSize bytes, always terminated. Returns the number of bytes used including the terminator. */
static uint64_t kbd_store(uint64_t string, const wchar_t *text, int utf8) {
    uint64_t ptr = rd64(string), cap = rd64(string + 8);
    if (!ptr || !cap) return 0;
    if (utf8) {
        char tmp[SWKBD_MAX_CHARS * 4 + 4];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, tmp, (int)sizeof tmp, NULL, NULL);
        if (n <= 0) { n = 1; tmp[0] = 0; }
        while ((uint64_t)n > cap) { n--; while (n > 1 && ((uint8_t)tmp[n - 1] & 0xc0) == 0x80) n--; if (n > 0) n--; tmp[n] = 0; n++; }
        memcpy((void *)(uintptr_t)ptr, tmp, (size_t)n);
        return (uint64_t)n;
    }
    uint64_t chars = wcslen(text);
    if ((chars + 1) * 2 > cap) chars = cap / 2 ? cap / 2 - 1 : 0;
    if (cap < 2) return 0;
    for (uint64_t i = 0; i < chars; i++) wr16(ptr + i * 2, (uint16_t)text[i]);
    wr16(ptr + chars * 2, 0);
    return (chars + 1) * 2;
}
static void swkbd_ShowKeyboard(Ctx *c) {
    uint64_t string = A0, arg = A1;
    static KbdDialog k;                                    /* one keyboard at a time, as on the console */
    static volatile LONG busy;
    static wchar_t header[70], sub[140], guide[260], initial[SWKBD_MAX_CHARS + 2], fixed[SWKBD_MAX_CHARS + 2];
    if (InterlockedExchange(&busy, 1)) { RET(SWKBD_RESULT_CANCELED); return; }
    copy_w(header, arg + KC_HEADER, 64);
    copy_w(sub, arg + KC_SUB, 128);
    copy_w(guide, arg + KC_GUIDE, 256);
    initial[0] = 0;
    uint64_t work = rd64(arg + ARG_WORKBUF);
    int init_len = (int)rd32(arg + KC_INITIAL_LENGTH), init_off = (int)rd32(arg + KC_INITIAL_OFFSET);
    if (work && init_len > 0 && init_len <= SWKBD_MAX_CHARS && init_off >= 0) copy_w(initial, work + (uint64_t)init_off, init_len);
    int utf8 = rd8(arg + KC_UTF8) != 0;
    int max_len = (int)rd32(arg + KC_MAXLEN), min_len = (int)rd32(arg + KC_MINLEN);
    if (max_len <= 0 || max_len > SWKBD_MAX_CHARS) max_len = SWKBD_MAX_CHARS;
    if (min_len < 0 || min_len > max_len) min_len = 0;
    uint64_t callback = rd8(arg + KC_CHECK_ENABLED) ? rd64(arg + KC_CHECK_CALLBACK) : 0, check_buf = rd64(arg + ARG_CHECKBUF);
    int mode = kbd_mode(fixed);
    uint32_t result = SWKBD_RESULT_CANCELED;
    trace_printf("     nn::swkbd::ShowKeyboard header \"%ls\" guide \"%ls\" max %d min %d%s\n", header, guide, max_len, min_len, callback ? " (text check)" : "");
    for (int attempt = 0; attempt < 16; attempt++) {
        int confirmed;
        if (mode == 1) confirmed = 0;
        else if (mode == 2) { wcsncpy(k.text, fixed, (size_t)max_len); k.text[max_len] = 0; confirmed = attempt == 0; }
        else {
            k.header = header; k.sub = sub; k.guide = guide; k.initial = initial;
            k.max_len = max_len; k.min_len = min_len; k.password = rd32(arg + KC_PASSWORD) == 1;
            confirmed = kbd_run_dialog(&k);
        }
        if (!confirmed) break;
        uint64_t bytes = kbd_store(string, k.text, utf8);
        int verdict = 0;
        if (callback && check_buf) {                       /* [sdk] 0x389ed8..0x389ef8 */
            uint64_t message_size = 0, saved_cap = rd64(string + 8), saved[12];
            for (int i = 0; i < 12; i++) saved[i] = c->x[19 + i];
            wr64(string + 8, bytes);
            c->x[0] = check_buf; c->x[1] = P(&message_size); c->x[2] = string;
            call_guest(c, callback);
            verdict = (int)(uint32_t)c->x[0];
            for (int i = 0; i < 12; i++) c->x[19 + i] = saved[i];
            wr64(string + 8, saved_cap);
            if (verdict) {
                wchar_t message[1024];
                if (utf8) { MultiByteToWideChar(CP_UTF8, 0, (const char *)(uintptr_t)check_buf, -1, message, 1023); message[1023] = 0; }
                else copy_w(message, check_buf, 1000);
                trace_printf("     nn::swkbd text check -> %d \"%ls\"\n", verdict, message);
                if (mode != 0) break;                      /* a scripted answer that is refused counts as cancelled */
                if (verdict == 2 && MessageBoxW(NULL, message, L"Software keyboard", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) verdict = 0;
                else if (verdict != 2) MessageBoxW(NULL, message, L"Software keyboard", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                if (verdict) { wcsncpy(initial, k.text, SWKBD_MAX_CHARS); initial[SWKBD_MAX_CHARS] = 0; continue; }
            }
        }
        result = 0;
        break;
    }
    if (result) trace_printf("     nn::swkbd::ShowKeyboard -> cancelled\n");
    else trace_printf("     nn::swkbd::ShowKeyboard -> \"%ls\"\n", k.text);
    InterlockedExchange(&busy, 0);
    RET(result);
}

/* Registration: no Odyssey NFC or help-guide hooks. */
static const ImportDef applet_imports[] = {
    {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", web_ShowOfflineHtmlPage},
    {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_GetExpectedVolumeBalance},
    {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", oe_SetExpectedVolumeBalance},
    {"_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_ShowKeyboard},
};
void register_nnapplet_imports(void) { register_imports(applet_imports, sizeof applet_imports / sizeof applet_imports[0]); }
