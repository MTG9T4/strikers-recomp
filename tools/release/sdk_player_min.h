/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minimal declarations for the current generated SDK lift, no Windows SDK. */
#include "runtime.h"
#define SDKLIFT_H
#define RECOMP_HOSTRT_H
typedef struct { uint64_t addr; void (*fn)(Ctx *); const char *name; } SdkFn;
typedef struct { const char *name; const unsigned char *bytes; unsigned size; } SdkData;
extern const SdkFn sdk_fns[];
extern const int sdk_fn_count;
extern const char *const sdk_host_names[];
extern const int sdk_host_name_count;
extern const uint64_t sdk_ctors[];
extern const int sdk_ctor_count;
extern const SdkData sdk_data[];
extern const int sdk_data_count;
void sdk_host_call(Ctx *,int);
void sdk_trap_call(Ctx *,const char *);
void sdk_dispatch(Ctx *,uint64_t);
