/* SPDX-License-Identifier: GPL-3.0-or-later
 * Project-owned bridge. Every pointer targets a DLL built locally by the player. */
#ifndef STRIKERS_PLAYER_SDK_ABI_H
#define STRIKERS_PLAYER_SDK_ABI_H
#include <stdint.h>
#define PLAYER_SDK_ABI 1u
typedef struct {
    unsigned abi, size;
    void (*indirect)(Ctx *, uint64_t);
    void (*import)(Ctx *, int);
    void (*trap)(const char *, uint64_t);
    void (*unimpl)(uint64_t, uint32_t, const char *);
    void (*svc)(Ctx *, uint32_t);
    void (*stx)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*sdk_host)(Ctx *, int);
    void (*sdk_trap)(Ctx *, const char *);
    void (*sdk_dispatch)(Ctx *, uint64_t);
} PlayerSdkHost;
typedef struct {
    unsigned abi, size;
    const void *functions;
    const int *function_count;
    const void *host_names;
    const int *host_name_count;
    const void *ctors;
    const int *ctor_count;
    const void *data;
    const int *data_count;
    const char *const *install_names;
    int install_count;
} PlayerSdkTables;
#endif
