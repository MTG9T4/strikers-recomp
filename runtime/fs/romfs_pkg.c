/* SPDX-License-Identifier: GPL-3.0-or-later
 * The game's RomFS comes only from the folder setup filled from the player's own extracted files (build\dump\update\romfs,
 * STRIKERS_ROMFS; installer/game_files.py). This project reads no game package and no console key: there is no other
 * source, and these are the empty answers nnfs.c and nnfsdir.c get for "a RomFS read from elsewhere". */
#include <stdint.h>

int romfs_pkg_active(void) { return 0; }
int romfs_pkg_init(void) { return 0; }
int romfs_pkg_lookup(const char *rel, int *is_dir, uint64_t *size) { (void)rel; (void)is_dir; (void)size; return -1; }
uint64_t romfs_pkg_size(int index) { (void)index; return 0; }
uint64_t romfs_pkg_read(int index, uint64_t offset, void *buf, uint64_t size) { (void)index; (void)offset; (void)buf; (void)size; return 0; }
int romfs_pkg_list(int dir, void (*each)(void *ctx, const char *name, int is_dir, uint64_t size), void *ctx) {
    (void)dir; (void)each; (void)ctx; return 0;
}
void romfs_pkg_counts(uint32_t *dirs, uint32_t *files, uint64_t *dir_bytes, uint64_t *file_bytes) {
    *dirs = *files = 0; *dir_bytes = *file_bytes = 0;
}
