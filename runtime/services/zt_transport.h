/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STRIKERS_ZT_TRANSPORT_H
#define STRIKERS_ZT_TRANSPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t id;
    int latency, path; /* -1 unknown, 0 relay, 1 direct */
} ZtPeerStatus;
typedef struct {
    int state; /* 0 offline, 1 joining, 2 connected, 3 waiting, 4 error */
    uint64_t node, network;
    char name[128], address[64], message[256];
    unsigned peers;
    ZtPeerStatus peer[64];
} ZtStatus;
int zt_requested(void);
void zt_config(char *mode, unsigned mode_cap, char *network, unsigned network_cap);
void zt_start(void); /* idempotent, immediately returns; background worker */
int zt_set_group_network(const char *network); /* validated enrollment; no active-network switching */
void zt_set_group_peers(const uint64_t *nodes, unsigned count); /* complete last-good directory, atomic replacement */
void zt_status(ZtStatus *status);
int zt_ready(void);
int zt_open(uint16_t base, uint16_t *bound);
void zt_close(void);
/* Host-order endpoint tokens are private to the SLDN bridge. Never guest IPs.
 * IPv6 addresses are mapped to stable bounded tokens; wire protocol unchanged. */
int zt_endpoint(uint32_t token);
int zt_send(uint32_t token, uint16_t port, const void *data, unsigned size);
int zt_recv(uint32_t *token, uint16_t *port, void *data, unsigned size);
void zt_discover(uint16_t base, const void *data, unsigned size);
/* The SLDN worker's wait for a datagram, at most ms milliseconds (audit 05). */
void zt_wait(unsigned ms);
#ifdef __cplusplus
}
#endif
#endif
