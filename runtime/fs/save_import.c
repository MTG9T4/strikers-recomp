/* SPDX-License-Identifier: GPL-3.0-or-later
 * Saves from emulators and console backups (see save_import.h).
 *
 * Where emulators keep this game's save (program 010019401051C000; the save is one file, strkrs.save):
 *   yuzu and its forks  %APPDATA%\<name>\nand\user\save\0000000000000000\<user id>\010019401051C000\strkrs.save
 *   Ryujinx             %APPDATA%\Ryujinx\bis\user\save\<save id>\0\strkrs.save  (the save id is per install; the file
 *                       name tells this game's save apart; folder 1 is Ryujinx's working copy)
 * The file format is the console's and is the same in all of them: a tagged container (magic F28555EE, version 2,
 * a record table, then the records end to end; tools/audit/inspect_save.py describes it). A file that fails that check
 * is never staged. Paths are UTF-8: the host runs with the UTF-8 code page (tools/release/host.manifest). */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save_import.h"
#pragma comment(lib, "comdlg32.lib")

extern const char *fs_user_root(void);
extern void *display_hwnd(void);

#define SAVE_MAX (1u << 20)            /* the game's save is about 8 KB; anything near this is not one */

static int read_file(const char *path, unsigned char **data, unsigned *size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER s; DWORD got = 0; int ok = 0;
    if (GetFileSizeEx(f, &s) && s.QuadPart > 0 && s.QuadPart <= SAVE_MAX) {
        *data = (unsigned char *)malloc((size_t)s.QuadPart);
        if (*data && ReadFile(f, *data, (DWORD)s.QuadPart, &got, NULL) && got == (DWORD)s.QuadPart) { *size = got; ok = 1; }
        else { free(*data); *data = NULL; }
    }
    CloseHandle(f);
    return ok;
}

static unsigned u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int check_bytes(const unsigned char *b, unsigned size, char *why, unsigned n) {
    if (size < 12) { snprintf(why, n, "too short to be a save (%u bytes)", size); return 0; }
    if (u32(b) != 0xf28555eeu || u32(b + 4) != 2) { snprintf(why, n, "not a Mario Strikers: Battle League save"); return 0; }
    unsigned count = u32(b + 8);
    if (count == 0 || count > 4096 || 12ull + count * 12ull > size) { snprintf(why, n, "damaged: the record table does not fit"); return 0; }
    unsigned start = 12 + count * 12, end = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned rsize = u32(b + 12 + i * 12 + 4), offset = u32(b + 12 + i * 12 + 8);
        if (offset != end || (unsigned long long)start + offset + rsize > size) { snprintf(why, n, "damaged: record %u is out of place", i); return 0; }
        end = offset + rsize;
    }
    if (start + end != size) { snprintf(why, n, "damaged: %u bytes after the last record", size - start - end); return 0; }
    snprintf(why, n, "%u records, %u bytes", count, size);
    return 1;
}

int save_import_check(const char *path, char *why, unsigned n) {
    unsigned char *b = NULL; unsigned size = 0;
    if (!read_file(path, &b, &size)) { snprintf(why, n, "cannot be read"); return 0; }
    int ok = check_bytes(b, size, why, n);
    free(b);
    return ok;
}

static void add(SaveCandidate *out, int max, int *count, const char *source, const char *path) {
    if (*count >= max) return;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    for (int i = 0; i < *count; i++) if (!_stricmp(out[i].path, path)) return;
    SaveCandidate *c = &out[(*count)++];
    memset(c, 0, sizeof *c);
    snprintf(c->source, sizeof c->source, "%s", source);
    snprintf(c->path, sizeof c->path, "%s", path);
    SYSTEMTIME utc, t;                     /* local time of that day, daylight saving included */
    FileTimeToSystemTime(&a.ftLastWriteTime, &utc); SystemTimeToTzSpecificLocalTime(NULL, &utc, &t);
    snprintf(c->when, sizeof c->when, "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    c->bytes = a.nFileSizeLow;
    char why[96];
    c->valid = save_import_check(path, why, sizeof why);
}

/* Each folder matching pattern (a FindFirstFile pattern ending in \*), then tail under it. */
static void each_dir(const char *parent, const char *tail, SaveCandidate *out, int max, int *count, const char *source) {
    char pattern[600]; WIN32_FIND_DATAA d;
    snprintf(pattern, sizeof pattern, "%s\\*", parent);
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.') continue;
        char path[600];
        snprintf(path, sizeof path, "%s\\%s\\%s", parent, d.cFileName, tail);
        add(out, max, count, source, path);
    } while (FindNextFileA(h, &d));
    FindClose(h);
}

int save_import_scan(SaveCandidate *out, int max) {
    int count = 0;
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        static const char *const yuzu_family[][2] = {
            {"yuzu", "yuzu"}, {"eden", "Eden"}, {"citron", "Citron"}, {"sudachi", "Sudachi"}, {"suyu", "Suyu"}, {"torzu", "Torzu"},
        };
        for (unsigned i = 0; i < sizeof yuzu_family / sizeof *yuzu_family; i++) {
            char users[600];
            snprintf(users, sizeof users, "%s\\%s\\nand\\user\\save\\0000000000000000", appdata, yuzu_family[i][0]);
            each_dir(users, "010019401051C000\\strkrs.save", out, max, &count, yuzu_family[i][1]);
        }
        char ryujinx[600];
        snprintf(ryujinx, sizeof ryujinx, "%s\\Ryujinx\\bis\\user\\save", appdata);
        each_dir(ryujinx, "0\\strkrs.save", out, max, &count, "Ryujinx");
    }
    /* Saves this copy kept when a save was brought in: the way back. */
    char backups[600], pattern[600]; WIN32_FIND_DATAA d;
    snprintf(backups, sizeof backups, "%s", fs_user_root());
    snprintf(pattern, sizeof pattern, "%s\\save-before-import-*", backups);
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char path[600];
            snprintf(path, sizeof path, "%s\\%s\\strkrs.save", backups, d.cFileName);
            add(out, max, &count, "Backup", path);
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
    return count;
}

static void staged_path(char *dir, unsigned n, const char *root) { snprintf(dir, n, "%s\\save-import", root); }

int save_import_stage(const char *path, char *message, unsigned n) {
    unsigned char *b = NULL; unsigned size = 0; char why[96];
    if (!read_file(path, &b, &size)) { snprintf(message, n, "That file cannot be read."); return 0; }
    if (!check_bytes(b, size, why, sizeof why)) { free(b); snprintf(message, n, "Not used: %s.", why); return 0; }
    char dir[600], file[640], temp[640];
    staged_path(dir, sizeof dir, fs_user_root());
    CreateDirectoryA(dir, NULL);
    snprintf(file, sizeof file, "%s\\strkrs.save", dir);
    snprintf(temp, sizeof temp, "%s\\strkrs.save.part", dir);
    HANDLE f = CreateFileA(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD put = 0; int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, b, size, &put, NULL) && put == size && FlushFileBuffers(f);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(b);
    ok = ok && MoveFileExA(temp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) { DeleteFileA(temp); snprintf(message, n, "The save could not be copied into this game's folder."); return 0; }
    printf("save-import: staged %s (%s); it is used from the next start\n", path, why);
    snprintf(message, n, "Ready: this save is used from the next start. Your current save is kept as a backup.");
    return 1;
}

int save_import_staged(void) {
    char dir[600], file[640];
    staged_path(dir, sizeof dir, fs_user_root());
    snprintf(file, sizeof file, "%s\\strkrs.save", dir);
    return GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
}

void save_import_cancel(void) {
    char dir[600], file[640];
    staged_path(dir, sizeof dir, fs_user_root());
    snprintf(file, sizeof file, "%s\\strkrs.save", dir);
    DeleteFileA(file); RemoveDirectoryA(dir);
    printf("save-import: the staged save was cancelled\n");
}

/* Called once from fs_roots: no save is mounted yet. A half-finished commit of the game's own save (save.commit) is
 * rolled forward by the next mount first; the staged save then waits one more start. */
void save_import_apply(const char *root) {
    char dir[600], file[640], save[600], marker[640], backup[640], why[96];
    staged_path(dir, sizeof dir, root);
    snprintf(file, sizeof file, "%s\\strkrs.save", dir);
    if (GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) return;
    snprintf(save, sizeof save, "%s\\save", root);
    snprintf(marker, sizeof marker, "%s.commit", save);
    if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) { printf("save-import: waiting: the game's last save is still being finished\n"); return; }
    if (!save_import_check(file, why, sizeof why)) {
        printf("save-import: the staged save is not used: %s\n", why);
        DeleteFileA(file); RemoveDirectoryA(dir);
        return;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(backup, sizeof backup, "%s\\save-before-import-%04u%02u%02u-%02u%02u%02u", root, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    DWORD a = GetFileAttributesA(save);
    if (a != INVALID_FILE_ATTRIBUTES && !MoveFileExA(save, backup, MOVEFILE_WRITE_THROUGH)) {
        printf("save-import: the current save could not be moved aside (%lu); the staged save waits\n", GetLastError());
        return;
    }
    if (!MoveFileExA(dir, save, MOVEFILE_WRITE_THROUGH)) {
        printf("save-import: the staged save could not be put in place (%lu)\n", GetLastError());
        if (a != INVALID_FILE_ATTRIBUTES) MoveFileExA(backup, save, MOVEFILE_WRITE_THROUGH);
        return;
    }
    printf("save-import: the imported save is in use (%s)%s%s\n", why, a != INVALID_FILE_ATTRIBUTES ? "; the save before it is in " : "",
           a != INVALID_FILE_ATTRIBUTES ? backup : "");
}

void save_import_restart(void) {
    char flag[600];
    snprintf(flag, sizeof flag, "%s\\restart-requested", fs_user_root());
    HANDLE f = CreateFileA(flag, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    printf("save-import: restarting to load the staged save\n");
    PostMessageA((HWND)display_hwnd(), WM_CLOSE, 0, 0);
}

/* The Open dialog runs on its own thread: the overlay keeps drawing while it is open. */
static volatile LONG g_pick_state;          /* 0 idle, 1 open, 2 chosen, 3 cancelled */
static char g_pick_path[520];
static DWORD WINAPI pick_thread(void *unused) {
    (void)unused;
    wchar_t file[520] = L"strkrs.save";
    OPENFILENAMEW o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = (HWND)display_hwnd();
    o.lpstrFilter = L"Mario Strikers save (strkrs.save)\0strkrs.save;*.save\0All files\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = 520; o.lpstrTitle = L"Choose a Mario Strikers: Battle League save";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&o) && WideCharToMultiByte(CP_UTF8, 0, file, -1, g_pick_path, sizeof g_pick_path, NULL, NULL))
        InterlockedExchange(&g_pick_state, 2);
    else InterlockedExchange(&g_pick_state, 3);
    return 0;
}
void save_import_pick_start(void) {
    if (InterlockedCompareExchange(&g_pick_state, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, pick_thread, NULL, 0, NULL);
    if (h) CloseHandle(h); else InterlockedExchange(&g_pick_state, 0);
}
int save_import_pick_poll(char *path, unsigned n) {
    LONG s = g_pick_state;
    if (s == 1) return -1;
    if (s == 2) { snprintf(path, n, "%s", g_pick_path); InterlockedExchange(&g_pick_state, 0); return 1; }
    if (s == 3) InterlockedExchange(&g_pick_state, 0);
    return 0;
}
