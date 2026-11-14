/* Probe for runtime/fs/save_import.c (tools/audit/test_save_import.py): the file check, staging, and the swap at start-up.
 * argv[1] is an empty work folder; argv[2] a real save to start from (any strkrs.save of this game). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static char g_root[MAX_PATH];
const char *fs_user_root(void) { return g_root; }
void *display_hwnd(void) { return NULL; }
#include "../../runtime/fs/save_import.c"

static int fails;
#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

static void put(const char *path, const unsigned char *b, size_t n) { FILE *f = fopen(path, "wb"); fwrite(b, 1, n, f); fclose(f); }
static unsigned char *get(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { *n = 0; return NULL; }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n + 1); fread(b, 1, *n, f); fclose(f); return b;
}
static int same(const char *a, const char *b) {
    size_t na, nb; unsigned char *x = get(a, &na), *y = get(b, &nb);
    int r = x && y && na == nb && !memcmp(x, y, na); free(x); free(y); return r;
}
static int is_dir_at(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

int main(int argc, char **argv) {
    if (argc != 3) return 64;
    snprintf(g_root, sizeof g_root, "%s", argv[1]);
    size_t n; unsigned char *real = get(argv[2], &n);
    if (!real) return 65;
    char path[MAX_PATH], why[96], msg[200];

    /* The check */
    snprintf(path, sizeof path, "%s\\good.save", g_root); put(path, real, n);
    CHECK(save_import_check(path, why, sizeof why), "a real save passes");
    unsigned char *bad = malloc(n + 8);
    memcpy(bad, real, n); bad[0] ^= 1;
    snprintf(path, sizeof path, "%s\\magic.save", g_root); put(path, bad, n);
    CHECK(!save_import_check(path, why, sizeof why), "a wrong magic number is refused");
    snprintf(path, sizeof path, "%s\\short.save", g_root); put(path, real, n - 1);
    CHECK(!save_import_check(path, why, sizeof why), "a cut-off save is refused");
    memcpy(bad, real, n); memcpy(bad + n, "EXTRA!!!", 8);
    snprintf(path, sizeof path, "%s\\long.save", g_root); put(path, bad, n + 8);
    CHECK(!save_import_check(path, why, sizeof why), "bytes after the last record are refused");
    memcpy(bad, real, n); bad[12 + 8] ^= 0x10;             /* first record's offset */
    snprintf(path, sizeof path, "%s\\table.save", g_root); put(path, bad, n);
    CHECK(!save_import_check(path, why, sizeof why), "a record out of place is refused");
    snprintf(path, sizeof path, "%s\\empty.save", g_root); put(path, real, 0);
    CHECK(!save_import_check(path, why, sizeof why), "an empty file is refused");
    snprintf(path, sizeof path, "%s\\missing.save", g_root);
    CHECK(!save_import_check(path, why, sizeof why), "a missing file is refused");

    /* Staging refuses a bad file and leaves nothing behind */
    snprintf(path, sizeof path, "%s\\magic.save", g_root);
    CHECK(!save_import_stage(path, msg, sizeof msg) && !save_import_staged(), "a bad file is not staged");

    /* No current save: the staged one becomes the save, no backup */
    snprintf(path, sizeof path, "%s\\good.save", g_root);
    CHECK(save_import_stage(path, msg, sizeof msg) && save_import_staged(), "a good file is staged");
    save_import_apply(g_root);
    char save[MAX_PATH]; snprintf(save, sizeof save, "%s\\save\\strkrs.save", g_root);
    CHECK(same(save, path) && !save_import_staged(), "first import: in place, staging gone");

    /* A current save: kept as a backup, which the scan lists */
    memcpy(bad, real, n); bad[n - 1] ^= 0xff;               /* a different but well-formed save: last payload byte */
    snprintf(path, sizeof path, "%s\\other.save", g_root); put(path, bad, n);
    CHECK(save_import_check(path, why, sizeof why), "a second save passes");
    char before[MAX_PATH]; snprintf(before, sizeof before, "%s\\before.save", g_root); CopyFileA(save, before, FALSE);
    save_import_stage(path, msg, sizeof msg);
    Sleep(1100);                                             /* backups are named to the second */
    save_import_apply(g_root);
    CHECK(same(save, path), "second import: in place");
    SaveCandidate c[24]; int count = save_import_scan(c, 24), backups = 0, backup_ok = 0;
    for (int i = 0; i < count; i++) if (!strcmp(c[i].source, "Backup")) { backups++; backup_ok = c[i].valid && same(c[i].path, before); }
    CHECK(backups == 1 && backup_ok, "the save before is listed as a backup, unchanged");

    /* An unfinished commit of the game's own save: the staged save waits */
    snprintf(path, sizeof path, "%s\\good.save", g_root);
    save_import_stage(path, msg, sizeof msg);
    char marker[MAX_PATH]; snprintf(marker, sizeof marker, "%s\\save.commit", g_root); put(marker, (const unsigned char *)"x", 1);
    save_import_apply(g_root);
    snprintf(path, sizeof path, "%s\\other.save", g_root);
    CHECK(same(save, path) && save_import_staged(), "with save.commit present nothing moves");
    DeleteFileA(marker);

    /* Cancel */
    save_import_cancel();
    CHECK(!save_import_staged() && !is_dir_at("save-import"), "cancel removes the staged save");

    /* A staged file damaged after staging is dropped at start-up */
    char staged_dir[MAX_PATH], staged[MAX_PATH];
    snprintf(staged_dir, sizeof staged_dir, "%s\\save-import", g_root); CreateDirectoryA(staged_dir, NULL);
    snprintf(staged, sizeof staged, "%s\\strkrs.save", staged_dir); put(staged, real, n - 3);
    save_import_apply(g_root);
    CHECK(same(save, path) && !save_import_staged(), "a damaged staged save is dropped, the save stays");

    printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
