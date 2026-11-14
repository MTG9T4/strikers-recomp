/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a bug report needs, in one place.
 *
 *  - system_summary_print(): the first line of every log: version, CPU level, Windows build, processor, memory and the
 *    game folder, so a log sent alone says which copy on which PC it came from.
 *  - F9 (display.c) -> bug_report_request(): the next presented frame is read back (nvn_vk_draw.c, as the Capture
 *    button's picture) and handed to bug_report_frame(); a worker thread then writes userdata\reports\report-<time>.zip
 *    with screenshot.jpg (at most 1920 wide), system.txt, the log so far, the run before, strikers.ini (invite code
 *    removed), controllers.ini and the newest crash report. With no frame within two seconds (another renderer, or the
 *    game not drawing) the report is written without the picture. The PC overlay shows where it went.
 * Nothing is sent anywhere: the player chooses whether to pass the file on. */
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

extern const char *fs_user_root(void);

static void own_folder(char *out, size_t n) {
    DWORD got = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (!got || got >= n) { snprintf(out, n, "."); return; }
    char *slash = strrchr(out, '\\'); if (slash) *slash = 0;
}

static void read_small(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "rb"); if (!f) return;
    size_t got = fread(out, 1, n - 1, f); fclose(f); out[got] = 0;
    while (got && (out[got - 1] == '\n' || out[got - 1] == '\r' || out[got - 1] == ' ')) out[--got] = 0;
}

static void release_version(char *out, size_t n) {
    char path[MAX_PATH]; own_folder(path, sizeof path);
    strncat(path, "\\release-manifest.json", sizeof path - strlen(path) - 1);
    snprintf(out, n, "(no release list)");
    FILE *f = fopen(path, "rb"); if (!f) return;
    static char buf[1 << 20]; size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[got] = 0;
    const char *k = strstr(buf, "\"version\""); if (!k) return;
    k = strchr(k + 9, '"'); if (!k) return;
    const char *e = strchr(k + 1, '"'); if (!e || e - k > 24) return;
    snprintf(out, n, "%.*s", (int)(e - k - 1), k + 1);
}

static void cpu_name(char *out, size_t n) {
    int r[12] = {0};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] < 0x80000004u) { snprintf(out, n, "(unknown processor)"); return; }
    __cpuid(r, 0x80000002); __cpuid(r + 4, 0x80000003); __cpuid(r + 8, 0x80000004);
    char s[49]; memcpy(s, r, 48); s[48] = 0;
    char *p = s; while (*p == ' ') p++;
    snprintf(out, n, "%s", p);
}

static void windows_build(char *out, size_t n) {
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v; memset(&v, 0, sizeof v); v.dwOSVersionInfoSize = sizeof v;
    RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (!get || get(&v)) { snprintf(out, n, "Windows (unknown build)"); return; }
    snprintf(out, n, "Windows %s build %lu", v.dwBuildNumber >= 22000 ? "11" : "10", (unsigned long)v.dwBuildNumber);
}

void system_summary(char *out, size_t n) {
    char version[32], level[32], cpu[64], win[48], folder[MAX_PATH], path[MAX_PATH];
    release_version(version, sizeof version);
    own_folder(folder, sizeof folder);
    snprintf(path, sizeof path, "%s\\build\\cpu-level.txt", folder);
    read_small(path, level, sizeof level);
    cpu_name(cpu, sizeof cpu);
    windows_build(win, sizeof win);
    MEMORYSTATUSEX m; m.dwLength = sizeof m; GlobalMemoryStatusEx(&m);
    snprintf(out, n, "Strikers Recomp %s | game code for %s | %s | %s, %lu logical processors | %.0f GB memory | folder %s",
             version, level[0] ? level : "(no cpu-level.txt)", win, cpu, (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
             (double)m.ullTotalPhys / (1024.0 * 1024 * 1024), folder);
}

void system_summary_print(void) {
    char s[700]; system_summary(s, sizeof s);
    printf("system: %s\n", s);
}

/* ---- F9 ---- */
static volatile LONG g_state;            /* 0 idle, 1 waiting for a frame, 2 writing */
static HANDLE g_frame_ready;
static unsigned char *g_px; static int g_w, g_h;
static CRITICAL_SECTION g_msg_lock; static volatile LONG g_msg_init;
static char g_message[400]; static ULONGLONG g_message_at;

static void set_message(const char *fmt, const char *arg) {
    if (InterlockedCompareExchange(&g_msg_init, 1, 0) == 0) { InitializeCriticalSection(&g_msg_lock); g_msg_init = 2; }
    while (g_msg_init != 2) Sleep(0);
    EnterCriticalSection(&g_msg_lock);
    snprintf(g_message, sizeof g_message, fmt, arg);
    g_message_at = GetTickCount64();
    LeaveCriticalSection(&g_msg_lock);
}

/* The overlay's toast: the text for 12 seconds after a report, else 0. */
int bug_report_message(char *out, size_t n) {
    if (g_msg_init != 2) return 0;
    EnterCriticalSection(&g_msg_lock);
    int on = g_message[0] && GetTickCount64() - g_message_at < 12000;
    if (on) snprintf(out, n, "%s", g_message);
    LeaveCriticalSection(&g_msg_lock);
    return on;
}

static int bug_report_waiting(void) { return g_state == 1; }
int bug_report_wants_frame(void) { return bug_report_waiting(); }

/* From the presenting thread: rgba, opaque, w x h. Copied; the caller keeps its buffer. */
void bug_report_frame(const unsigned char *rgba, int w, int h) {
    if (g_state != 1 || g_px) return;
    unsigned char *copy = malloc((size_t)w * h * 4);
    if (!copy) return;
    memcpy(copy, rgba, (size_t)w * h * 4);
    g_w = w; g_h = h; g_px = copy;
    SetEvent(g_frame_ready);
}

static int write_jpeg(const char *path, const unsigned char *rgba, int w, int h) {
    int ow = w, oh = h;
    if (ow > 1920) { oh = (int)((long long)h * 1920 / w); ow = 1920; }
    /* box-filtered down to ow x oh, as BGR for WIC */
    unsigned char *bgr = malloc((size_t)ow * oh * 3);
    if (!bgr) return 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int x0 = (int)((long long)x * w / ow), x1 = (int)((long long)(x + 1) * w / ow), y0 = (int)((long long)y * h / oh), y1 = (int)((long long)(y + 1) * h / oh);
            if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
            unsigned r = 0, g = 0, b = 0, c = 0;
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const unsigned char *s = rgba + ((size_t)yy * w + xx) * 4; r += s[0]; g += s[1]; b += s[2]; c++; }
            unsigned char *d = bgr + ((size_t)y * ow + x) * 3; d[0] = (unsigned char)(b / c); d[1] = (unsigned char)(g / c); d[2] = (unsigned char)(r / c);
        }
    wchar_t wpath[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *f = NULL; IWICStream *s = NULL; IWICBitmapEncoder *e = NULL; IWICBitmapFrameEncode *fr = NULL; IPropertyBag2 *props = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) && SUCCEEDED(IWICStream_InitializeFromFilename(s, wpath, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &e)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(e, &fr, &props))) {
        PROPBAG2 opt; memset(&opt, 0, sizeof opt); opt.pstrName = L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.88f;
        IPropertyBag2_Write(props, 1, &opt, &v);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props)) && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, ow, oh)) &&
             SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt)) && IsEqualGUID(&fmt, &GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, oh, ow * 3, ow * 3 * oh, bgr)) &&
             SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(e));
    }
    if (props) IPropertyBag2_Release(props); if (fr) IWICBitmapFrameEncode_Release(fr); if (e) IWICBitmapEncoder_Release(e);
    if (s) IWICStream_Release(s); if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(co)) CoUninitialize();
    free(bgr);
    return ok;
}

/* A copy that the game may still be writing to (the log): read with every share mode, written whole. */
static int copy_shared(const char *from, const char *to) {
    HANDLE in = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    HANDLE out = CreateFileA(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
    static char buf[1 << 16]; DWORD got, put; int ok = 1;
    while (ReadFile(in, buf, sizeof buf, &got, NULL) && got) if (!WriteFile(out, buf, got, &put, NULL) || put != got) { ok = 0; break; }
    CloseHandle(in); CloseHandle(out);
    return ok;
}

/* strikers.ini with the private invite code blanked. */
static void copy_settings(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return; }
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!_strnicmp(p, "group_code", 10) && strchr(p, '=')) { const char *eq = strchr(p, '=');
            fprintf(out, "%.*s= (removed from the report)\r\n", (int)(eq - line), line); }
        else fputs(line, out);
    }
    fclose(in); fclose(out);
}

static void newest(const char *dir, const char *pattern, char *out, size_t n) {
    char find[MAX_PATH]; WIN32_FIND_DATAA d; FILETIME best = {0, 0};
    out[0] = 0;
    snprintf(find, sizeof find, "%s\\%s", dir, pattern);
    HANDLE h = FindFirstFileA(find, &d); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CompareFileTime(&d.ftLastWriteTime, &best) > 0) { best = d.ftLastWriteTime; snprintf(out, n, "%s\\%s", dir, d.cFileName); }
    } while (FindNextFileA(h, &d));
    FindClose(h);
}

static DWORD WINAPI report_worker(void *unused) {
    (void)unused;
    WaitForSingleObject(g_frame_ready, 2000);
    InterlockedExchange(&g_state, 2);
    const char *user = fs_user_root();
    char reports[MAX_PATH], dir[MAX_PATH], zip[MAX_PATH], path[MAX_PATH], from[MAX_PATH], folder[MAX_PATH], name[64];
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(name, sizeof name, "report-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    snprintf(reports, sizeof reports, "%s\\reports", user); CreateDirectoryA(reports, NULL);
    snprintf(dir, sizeof dir, "%s\\%s", reports, name); CreateDirectoryA(dir, NULL);
    own_folder(folder, sizeof folder);
    int picture = 0;
    if (g_px) { snprintf(path, sizeof path, "%s\\screenshot.jpg", dir); picture = write_jpeg(path, g_px, g_w, g_h); free(g_px); g_px = NULL; }
    char summary[700]; system_summary(summary, sizeof summary);
    snprintf(path, sizeof path, "%s\\system.txt", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "%s\r\nReport made %04u-%02u-%02u %02u:%02u:%02u with F9%s.\r\n", summary, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                picture ? "" : " (no screenshot: the game was not drawing)");
        fclose(f);
    }
    snprintf(from, sizeof from, "%s\\last-game.log", user); snprintf(path, sizeof path, "%s\\last-game.log", dir); copy_shared(from, path);
    snprintf(from, sizeof from, "%s\\previous-game.log", user); snprintf(path, sizeof path, "%s\\previous-game.log", dir); copy_shared(from, path);
    snprintf(from, sizeof from, "%s\\controllers.ini", user); snprintf(path, sizeof path, "%s\\controllers.ini", dir); copy_shared(from, path);
    snprintf(from, sizeof from, "%s\\strikers.ini", folder); snprintf(path, sizeof path, "%s\\strikers.ini", dir); copy_settings(from, path);
    char crashes[MAX_PATH]; snprintf(crashes, sizeof crashes, "%s\\crashes", user);
    newest(crashes, "*", from, sizeof from);
    if (from[0]) { const char *leaf = strrchr(from, '\\'); snprintf(path, sizeof path, "%s\\%s", dir, leaf ? leaf + 1 : "crash"); copy_shared(from, path); }
    /* one file to send: Windows' own tar writes zip archives (-a picks the format from the name) */
    snprintf(zip, sizeof zip, "%s\\%s.zip", reports, name);
    char tar[MAX_PATH], cmd[3 * MAX_PATH];
    GetSystemDirectoryA(tar, sizeof tar); strncat(tar, "\\tar.exe", sizeof tar - strlen(tar) - 1);
    snprintf(cmd, sizeof cmd, "\"%s\" -a -c -f \"%s.zip\" \"%s\"", tar, name, name);
    STARTUPINFOA si; PROCESS_INFORMATION p; memset(&si, 0, sizeof si); si.cb = sizeof si;
    DWORD code = 1;
    if (CreateProcessA(tar, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, reports, &si, &p)) {
        WaitForSingleObject(p.hProcess, 60000); GetExitCodeProcess(p.hProcess, &code); CloseHandle(p.hThread); CloseHandle(p.hProcess);
    }
    if (code == 0 && GetFileAttributesA(zip) != INVALID_FILE_ATTRIBUTES) {
        WIN32_FIND_DATAA d; char find[MAX_PATH]; snprintf(find, sizeof find, "%s\\*", dir);
        HANDLE h = FindFirstFileA(find, &d);
        if (h != INVALID_HANDLE_VALUE) { do { if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { snprintf(path, sizeof path, "%s\\%s", dir, d.cFileName); DeleteFileA(path); } } while (FindNextFileA(h, &d)); FindClose(h); }
        RemoveDirectoryA(dir);
        printf("bug-report: %s (screenshot %s)\n", zip, picture ? "yes" : "no");
        char shown[160]; snprintf(shown, sizeof shown, "userdata\\reports\\%s.zip", name);
        set_message("Bug report saved in the game folder: %s. Send this file to your organizer if you choose; nothing is sent automatically.", shown);
    } else {
        printf("bug-report: %s (folder; zip failed, exit %lu)\n", dir, (unsigned long)code);
        char shown[160]; snprintf(shown, sizeof shown, "userdata\\reports\\%s", name);
        set_message("Bug report saved in the game folder: %s. Send it to your organizer if you choose.", shown);
    }
    InterlockedExchange(&g_state, 0);
    return 0;
}

void bug_report_request(void) {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;     /* one at a time */
    if (!g_frame_ready) g_frame_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    ResetEvent(g_frame_ready);
    set_message("Saving a bug report%s", "...");
    printf("bug-report: requested\n");
    HANDLE h = CreateThread(NULL, 0, report_worker, NULL, 0, NULL);
    if (h) CloseHandle(h); else InterlockedExchange(&g_state, 0);
}
