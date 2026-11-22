/* SPDX-License-Identifier: GPL-3.0-or-later
 * Internet rooms: Local Wireless through the Strikers relay (server/relay/strikers_relay.py, protocol described there).
 * Runs beside the same-network socket in nnldn_lan.inc. Relay peers are addressed by the server's 24-bit session
 * tokens, kept as IPv4 words in 0.0.0.0/8 (never a real source address). */
#ifndef STRIKERS_RELAY_TRANSPORT_H
#define STRIKERS_RELAY_TRANSPORT_H
#include <winsock2.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RELAY_DEFAULT_PORT 24890
#define RELAY_IS_TOKEN(ip) ((ip)!=0 && ((ip)>>24)==0)
typedef struct {
    int state;          /* 0 off, 1 connecting, 2 connected, 3 unreachable */
    int ping_ms;        /* -1 unknown */
    int rooms;          /* rooms the last search found on the relay, -1 not searched */
    int keyed;          /* a room key is set */
    char server[128];   /* host[:port] */
    char message[192];
} RelayStatus;
/* [Network] mode = internet (the default) */
int relay_requested(void);
/* nnldn's LDN lock is held for all of these except relay_status and relay_set_room_key. Lock order: LDN, then relay. */
int relay_open(uint64_t build);     /* returns at once; the name is resolved on a thread of its own */
void relay_close(void);
SOCKET relay_socket(void);          /* for select(); INVALID_SOCKET while closed */
void relay_tick(uint64_t now_ms);   /* HELLO, PING and reconnects; call every few hundred ms */
/* The next SLDN datagram from the relay (a peer's DATA, or a ROOM answer as that host's advert): its size, -1 none. */
int relay_recv(uint32_t *token, void *data, unsigned cap);
int relay_send(uint32_t token, const void *data, unsigned size);
int relay_endpoint(uint32_t token); /* a token the relay may carry: connected and a valid token */
void relay_host(const void *advert, unsigned size);
void relay_unhost(void);
void relay_list(void);
/* F1: any thread */
void relay_status(RelayStatus *out);
void relay_set_room_key(const char *key); /* a new session with this key's room; "" = public */
void relay_room_key(char *out, unsigned cap);
#ifdef __cplusplus
}
#endif
#endif
