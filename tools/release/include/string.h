/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minimal Windows x64 C ABI declarations; no Windows SDK headers required. */
#ifndef STRIKERS_PLAYER_STRING_H
#define STRIKERS_PLAYER_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#endif
