/* GPL-3.0-or-later. Real production journal, isolated process-kill harness. */
#include "../../runtime/fs/nnfs.c"
void register_imports(const ImportDef *d, int n) { (void)d; (void)n; }
void trace_printf(const char *s, ...) { (void)s; }
void input_observe(const char *k, const char *d) { (void)k; (void)d; }
void hitch_note(const char *s) { (void)s; }
int g_hitch_ms;
int main(int argc, char **argv) {
    Ctx c = {0}; uint64_t uid[2] = {1, 0};
    if (argc == 2 && !strcmp(argv[1], "selftest")) return nnfs_selftest(&c);
    if (argc != 3) return 2;
    _putenv_s("STRIKERS_USERDATA", argv[1]);
    c.x[0] = (uint64_t)(uintptr_t)"st"; c.x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(&c);
    if (c.x[0]) return 3;
    if (!strcmp(argv[2], "write") || !strcmp(argv[2], "flushfail")) {
        if (st_write(&c, "st:/strkrs.save", "NEW SAVE", NULL)) return 4;
        /* Set up the journal's documented D directory / F child case. */
        tomb_add(mount_find("st", 2), "nested");
        c.x[0] = (uint64_t)(uintptr_t)"st:/nested"; fs_CreateDirectory(&c);
        if (c.x[0]) return 6;
        c.x[0] = (uint64_t)(uintptr_t)"st:/nested/new.bin"; c.x[1] = 8; fs_CreateFile(&c);
        if (c.x[0] || st_write(&c, "st:/nested/new.bin", "NEW FILE", NULL)) return 7;
        HANDLE blocked = INVALID_HANDLE_VALUE;
        if (!strcmp(argv[2], "flushfail")) {
            char host[MAX_PATH]; Mount *m = mount_find("st", 2);
            join(host, m->work, "strkrs.save");
            blocked = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (blocked == INVALID_HANDLE_VALUE) return 10;
        }
        c.x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(&c);
        if (blocked != INVALID_HANDLE_VALUE) { CloseHandle(blocked); if (!c.x[0]) return 11; c.x[0] = 0; }
        if (c.x[0]) return 8;
    } else if (!strcmp(argv[2], "readopen")) {
        /* Audit 03: the game holds the committed file open for reading while it commits (allowed by the SDK: only an
         * open writer refuses Commit). The commit must succeed; the reader keeps the contents it opened. */
        uint64_t rh = 0, got = 0; char buf[16] = "";
        c.x[0] = (uint64_t)(uintptr_t)&rh; c.x[1] = (uint64_t)(uintptr_t)"st:/strkrs.save"; c.x[2] = 1; fs_OpenFile(&c);
        if (c.x[0]) return 12;
        if (st_write(&c, "st:/strkrs.save", "NEW SAVE", NULL)) return 13;
        c.x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(&c);
        uint64_t committed = c.x[0];
        c.x[0] = (uint64_t)(uintptr_t)&got; c.x[1] = rh; c.x[2] = 0; c.x[3] = (uint64_t)(uintptr_t)buf; c.x[4] = 8; fs_ReadFileCount(&c);
        c.x[0] = rh; fs_CloseFile(&c);
        if (committed) return 14;
        if (strcmp(buf, "OLD SAVE")) return 15;
        if (!st_read_is(&c, "st:/strkrs.save", "NEW SAVE")) return 16;
    } else if (strcmp(argv[2], "recover")) return 9;
    c.x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(&c);
    return 0;
}
