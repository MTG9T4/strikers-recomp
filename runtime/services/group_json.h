/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef STRIKERS_GROUP_JSON_H
#define STRIKERS_GROUP_JSON_H
/* Bounded JSON tree; end is the index after the complete subtree. */
typedef struct { int start,length,end,kind; } GjToken;
int gj_parse(const char *s,GjToken *tokens,int capacity);
int gj_field(const char *s,const GjToken *t,int object,const char *key);
int gj_string(const char *s,const GjToken *t,int i,char *out,unsigned cap);
#endif
