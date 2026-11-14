/* SPDX-License-Identifier: GPL-3.0-or-later
 * RomFS mods, in the layout yuzu and Ryujinx use: <game folder>\mods\<mod name>\romfs\<path inside the RomFS>.
 * A mod's file is read instead of the game's file of the same path (or added, when the game has none); folders a mod
 * adds are listed. Mods are taken in name order and the first one that has a file wins. A mod is off while its folder
 * has a file named "disabled" (Settings > Mods switches that). Read once at start-up; nothing of the game is changed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[128]; char root[MAX_PATH]; } Mod;
static Mod *g_mods; static int g_nmods, g_inited;

static int mod_cmp(const void *a, const void *b) { return _stricmp(((const Mod *)a)->name, ((const Mod *)b)->name); }

void mods_folder(char *out, size_t n) {
    char exe[MAX_PATH]; DWORD got = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = got && got < MAX_PATH ? strrchr(exe, '\\') : NULL;
    if (slash) *slash = 0; else snprintf(exe, sizeof exe, ".");
    const char *env = getenv("STRIKERS_MODS");
    snprintf(out, n, "%s", env && env[0] ? env : exe);
    if (!(env && env[0])) strncat(out, "\\mods", n - strlen(out) - 1);
}

int mods_init(void) {
    if (g_inited) return g_nmods;
    g_inited = 1;
    char base[MAX_PATH], pat[MAX_PATH + 4]; WIN32_FIND_DATAA d;
    mods_folder(base, sizeof base);
    snprintf(pat, sizeof pat, "%s\\*", base);
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.') continue;
        char romfs[MAX_PATH], off[MAX_PATH];
        snprintf(romfs, sizeof romfs, "%s\\%s\\romfs", base, d.cFileName);
        snprintf(off, sizeof off, "%s\\%s\\disabled", base, d.cFileName);
        DWORD a = GetFileAttributesA(romfs);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (GetFileAttributesA(off) != INVALID_FILE_ATTRIBUTES) { printf("mods: %s is switched off\n", d.cFileName); continue; }
        g_mods = realloc(g_mods, sizeof(Mod) * (size_t)(g_nmods + 1));
        snprintf(g_mods[g_nmods].name, sizeof g_mods[g_nmods].name, "%s", d.cFileName);
        snprintf(g_mods[g_nmods].root, sizeof g_mods[g_nmods].root, "%s", romfs);
        g_nmods++;
    } while (FindNextFileA(h, &d));
    FindClose(h);
    qsort(g_mods, (size_t)g_nmods, sizeof(Mod), mod_cmp);
    for (int i = 0; i < g_nmods; i++) printf("mods: %s is on (its files are used instead of the game's)\n", g_mods[i].name);
    return g_nmods;
}

static void host_of(const Mod *m, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s%s%s", m->root, rel[0] ? "\\" : "", rel);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}

/* The host file a mod puts at RomFS path rel ('\' or '/'), 1 if one does; is_dir: a folder of a mod. */
int mods_find(const char *rel, char *host, size_t n, int *is_dir) {
    if (!mods_init()) return 0;
    for (int i = 0; i < g_nmods; i++) {
        char p[MAX_PATH]; host_of(&g_mods[i], rel, p, sizeof p);
        DWORD a = GetFileAttributesA(p);
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (is_dir) *is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (host) snprintf(host, n, "%s", p);
        return 1;
    }
    return 0;
}

/* Every entry the mods have in RomFS folder rel (each name once, the first mod's). */
void mods_list(const char *rel, void (*each)(void *ctx, const char *name, int is_dir, unsigned long long size), void *ctx) {
    if (!mods_init()) return;
    char seen[256][128]; int nseen = 0;
    for (int i = 0; i < g_nmods; i++) {
        char dir[MAX_PATH], pat[MAX_PATH + 4]; WIN32_FIND_DATAA d;
        host_of(&g_mods[i], rel, dir, sizeof dir);
        snprintf(pat, sizeof pat, "%s\\*", dir);
        HANDLE h = FindFirstFileA(pat, &d);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, "..")) continue;
            int dup = 0; for (int k = 0; k < nseen && !dup; k++) dup = !_stricmp(seen[k], d.cFileName);
            if (dup) continue;
            if (nseen < 256) snprintf(seen[nseen++], 128, "%s", d.cFileName);
            each(ctx, d.cFileName, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, (unsigned long long)d.nFileSizeHigh << 32 | d.nFileSizeLow);
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
}

/* Settings > Mods: every mod folder (on or off), and switching one. */
int mods_all(char names[][128], int *on, int max) {
    char base[MAX_PATH], pat[MAX_PATH + 4]; WIN32_FIND_DATAA d; int n = 0;
    mods_folder(base, sizeof base);
    snprintf(pat, sizeof pat, "%s\\*", base);
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.' || n >= max) continue;
        char romfs[MAX_PATH], off[MAX_PATH];
        snprintf(romfs, sizeof romfs, "%s\\%s\\romfs", base, d.cFileName);
        snprintf(off, sizeof off, "%s\\%s\\disabled", base, d.cFileName);
        if (GetFileAttributesA(romfs) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(names[n], 128, "%s", d.cFileName);
        on[n] = GetFileAttributesA(off) == INVALID_FILE_ATTRIBUTES;
        n++;
    } while (FindNextFileA(h, &d));
    FindClose(h);
    return n;
}
void mods_set(const char *name, int on) {
    char base[MAX_PATH], off[MAX_PATH];
    mods_folder(base, sizeof base);
    snprintf(off, sizeof off, "%s\\%s\\disabled", base, name);
    if (on) DeleteFileA(off);
    else { HANDLE f = CreateFileA(off, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); if (f != INVALID_HANDLE_VALUE) CloseHandle(f); }
    printf("mods: %s switched %s (from the next start)\n", name, on ? "on" : "off");
}
