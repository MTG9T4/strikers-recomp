/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strikers project code. Offline results follow smo-recomp/services/nnonline.inc
 * and ssbu-recomp/runtime/nnnet_c4a.inc. LAN contracts/evidence: docs/phase6-lan.md. */
#ifndef STRIKERS_NETWORK_POLICY_H
#define STRIKERS_NETWORK_POLICY_H
#include "hostrt.h"
#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define PTR(v) ((void *)(uintptr_t)(v))
#define ACC_CANCELLED 0x27cu
#define ACC_NO_INTERNET 0x767cu
#define FR_NOT_CALLED 0xe79u
void register_nnaccount_imports(void);
void register_nnfriends_imports(void);
void register_nnldn_imports(void);
void register_nnsocket_imports(void);
void register_nnnifm_imports(void);
void register_nnssl_imports(void);
/* Install a complete replacement before boot. Nonzero STRIKERS_NETWORK requests
 * that replacement; a request without a module fails explicitly. No public host,
 * DNS override, lobby, thread or transport is inherited from either upstream. */
typedef void (*StrikersNetworkModule)(void);
void strikers_set_network_module(StrikersNetworkModule module);
void register_network_imports(void);
/* LAN mode never enables nifm, DNS, TLS, account login, or public sockets. */
int strikers_lan_enabled(void);
/* Phase 7 local development gate. Numeric loopback only, no host resolver.
 * Immutable process configuration; INI [Network] keys are documented in server/nex. */
typedef struct StrikersOnlineConfig {
    uint32_t server_ip;
    uint16_t ports[8];
    unsigned port_count;
    char token_file[1024], ca_file[1024], openssl_file[1024];
} StrikersOnlineConfig;
int strikers_online_requested(void);
int strikers_online_enabled(void);
const StrikersOnlineConfig *strikers_online_config(void);
int strikers_online_destination(uint32_t ip, uint16_t port);
int strikers_online_hostname(const char *name);
/* Addresses here and in LDN NodeInfo are numeric host-order IPv4 words. */
uint32_t ldn_local_ip(void);
int ldn_udp_send(uint32_t to, uint16_t source_port, uint16_t port, const void *data, unsigned size);
void socket_ldn_receive(uint32_t from, uint32_t to, uint16_t source_port, uint16_t port, const void *data, unsigned size);
void socket_ldn_changed(void);
#endif
