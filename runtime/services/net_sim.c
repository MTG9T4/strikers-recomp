/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_sim.h"
static struct {uint64_t due;uint32_t ip;uint16_t port,size;unsigned char data[4160];} queue[4096];
static unsigned delay,jitter,loss,random_state;
/* Audit 05: the earliest due time of a queued datagram (UINT64_MAX: none), kept under the SLDN lock with the queue and read
 * without it by net_sim_wait (one aligned 64-bit word: a stale value only moves one wake-up). */
static volatile uint64_t next_due=UINT64_MAX;
static unsigned rng(void) {random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
void net_sim_clear(void) {if(next_due!=UINT64_MAX)for(unsigned i=0;i<4096;i++)queue[i].size=0;next_due=UINT64_MAX;}   /* 17 MB: touched only when something is queued */
unsigned net_sim_wait(uint64_t now,unsigned cap) {uint64_t due=next_due;return due==UINT64_MAX||due>=now+cap?cap:due<=now?0:(unsigned)(due-now);}
int net_sim_init(const char *spec) {
    net_sim_clear();delay=jitter=loss=0;random_state=0x7354726b;
    if(!spec||!*spec)return 1;
    unsigned d,j,l;char tail;
    if(sscanf(spec,"%u,%u,%u%c",&d,&j,&l,&tail)!=3||d>5000||j>5000||l>100)return 0;
    delay=d;jitter=j;loss=l;
    printf("[net-sim] one-way=%u jitter=+/- %u ms loss=%u%% queue=4096\n",delay,jitter,loss);return 1;
}
int net_sim_send(uint64_t now,uint32_t ip,uint16_t port,const void *data,unsigned size,NetSimSend send) {
    if(size>4160||!size)return -1;
    if(!delay&&!jitter&&!loss)return send(ip,port,data,size);
    if(loss&&rng()%100<loss)return (int)size;
    int ms=(int)delay+(jitter?(int)(rng()%(2*jitter+1))-(int)jitter:0);if(ms<0)ms=0;
    for(unsigned i=0;i<4096;i++)if(!queue[i].size) {
        queue[i].due=now+ms;queue[i].ip=ip;queue[i].port=port;queue[i].size=(uint16_t)size;memcpy(queue[i].data,data,size);
        if(queue[i].due<next_due)next_due=queue[i].due;return (int)size;
    }
    /* Bounded congestion is a dropped UDP datagram, not a blocking send. */
    return (int)size;
}
void net_sim_flush(uint64_t now,NetSimSend send) {
    if(next_due>now)return;                        /* audit 05: nothing due (also: the simulator is off) */
    uint64_t next=UINT64_MAX;
    for(unsigned i=0;i<4096;i++)if(queue[i].size) {
        if(queue[i].due<=now){send(queue[i].ip,queue[i].port,queue[i].data,queue[i].size);queue[i].size=0;}
        else if(queue[i].due<next)next=queue[i].due;
    }
    next_due=next;
}
