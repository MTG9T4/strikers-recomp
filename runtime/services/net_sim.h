/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STRIKERS_NET_SIM_H
#define STRIKERS_NET_SIM_H
#include <stdint.h>
typedef int (*NetSimSend)(uint32_t ip,uint16_t port,const void *data,unsigned size);
/* Single SLDN lock owns this queue. Never sleep a game thread. */
int net_sim_init(const char *spec);
int net_sim_send(uint64_t now,uint32_t ip,uint16_t port,const void *data,unsigned size,NetSimSend send);
void net_sim_flush(uint64_t now,NetSimSend send);
void net_sim_clear(void);
/* Milliseconds until the next queued datagram is due, at most cap (cap when none): how long the SLDN worker may wait. */
unsigned net_sim_wait(uint64_t now,unsigned cap);
#endif
