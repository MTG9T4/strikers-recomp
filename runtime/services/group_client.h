/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STRIKERS_GROUP_CLIENT_H
#define STRIKERS_GROUP_CLIENT_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {char node[11],name[129],status[16];} GroupFriend;
typedef struct {
    int configured,joined,busy,reachable,hosting,in_match,revoked;
    char message[256],code[16],name[129],self[11],toast[256],invite_id[37];
    unsigned count;GroupFriend friends[128];
} GroupStatus;
void group_start(void); /* once, background I/O; safe from frame thread */
void group_status(GroupStatus *s);
void group_connection(char *text,unsigned capacity); /* plain UI status, no IDs */
void group_join(const char *code,const char *name);
void group_invite(const char *node);
void group_dismiss(void);
void group_notify(void (*notify)(void)); /* UI callback: no group calls/locks */
void group_ldn_state(int state); /* cheap atomics; no network I/O */
void group_match(int active); /* explicit user selection until a verified match boundary exists */
#ifdef __cplusplus
}
#endif
#endif
