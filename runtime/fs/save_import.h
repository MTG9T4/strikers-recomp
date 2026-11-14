/* SPDX-License-Identifier: GPL-3.0-or-later
 * Saves from emulators (Ryujinx, yuzu and its forks such as Eden) or a console backup, brought into <userdata>\save.
 * The PC settings list what was found; a chosen save is checked and staged in <userdata>\save-import, and the next
 * start puts it in place before the game mounts its save (the save in use moves to <userdata>\save-before-import-*). */
#ifndef SAVE_IMPORT_H
#define SAVE_IMPORT_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char source[32];          /* "Ryujinx", "yuzu", "Eden", ..., "Backup" */
    char path[520];           /* UTF-8, the strkrs.save file */
    char when[32];            /* last written, local time, "2023-09-17 22:55" */
    unsigned bytes;
    int valid;                /* the file is a Mario Strikers: Battle League save */
} SaveCandidate;

int save_import_scan(SaveCandidate *out, int max);          /* how many were found (at most max) */
int save_import_check(const char *path, char *why, unsigned n); /* 1: a save this game can read */
int save_import_stage(const char *path, char *message, unsigned n); /* 1: staged for the next start */
int save_import_staged(void);                                /* 1: a staged save waits for the next start */
void save_import_cancel(void);
void save_import_apply(const char *user_root);               /* at start-up, before any save is mounted */
void save_import_restart(void);                              /* closes the game; the launcher starts it again */
void save_import_pick_start(void);                           /* opens the Open dialog on its own thread */
int save_import_pick_poll(char *path, unsigned n);           /* -1: still open, 1: a file was chosen, 0: none */

#ifdef __cplusplus
}
#endif
#endif
