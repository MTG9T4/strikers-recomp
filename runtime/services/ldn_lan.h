/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strikers LAN transport. ABI/state-machine reference: Eden/yuzu
 * src/core/hle/service/ldn/{ldn_types.h,lan_discovery.cpp}, GPL, authors
 * yuzu Emulator Project (2022), Eden Emulator Project (2026).
 * Independently written bounded UDP transport; not Eden wire-compatible. */
#ifndef STRIKERS_LDN_LAN_H
#define STRIKERS_LDN_LAN_H
#define LDN_NI_SIZE 0x480
#define LDN_NODE(i) (0x68 + 0x40 * (i))
#define LDN_MAX_DATA 4096
#define LDN_WIRE_HEADER 64
#define LDN_PORT_SLOTS 16
#define LDN_DEFAULT_PORT 24872
#define LDN_WIRE_VERSION 1
#define LDN_BUILD_ID 0x2e90f0815b1f92a1ULL
#if __has_include("ldn_fingerprint.h")
#include "ldn_fingerprint.h"
#endif
#ifndef STRIKERS_LDN_BUILD_DEFAULT
#define STRIKERS_LDN_BUILD_DEFAULT LDN_BUILD_ID
#endif
#define LDN_TIMEOUT_MS 6000
#define LDN_R_STATE 0x40cbu
#define LDN_R_CONNECT 0x3ecbu
#define LDN_R_INVALID 0xc0cbu
#define LDN_R_AUTH 0x28cbu
#define LDN_R_TIMEOUT 0x84cbu
#define LDN_R_FULL 0x86cbu
#define LDN_R_VERSION_LOW 0xe2cbu
#define LDN_R_VERSION_HIGH 0xe4cbu
enum { LDN_NONE, LDN_INIT, LDN_AP, LDN_CREATED, LDN_STA, LDN_CONNECTED };
enum { LP_SCAN=1, LP_ADVERT, LP_JOIN, LP_SYNC, LP_REFUSE, LP_KEEP,
       LP_LEAVE, LP_DESTROY, LP_KICK, LP_DATA };
#endif
