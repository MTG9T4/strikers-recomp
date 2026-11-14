/* SPDX-License-Identifier: GPL-3.0-or-later
 * Release-host loader; no generated SDK code or byte arrays are linked here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "sdklift.h"
#include "sdk_abi.h"
/* Undo the compile-time remaps for the actual pointer definitions. */
#undef sdk_fns
#undef sdk_fn_count
#undef sdk_host_names
#undef sdk_host_name_count
#undef sdk_ctors
#undef sdk_ctor_count
#undef sdk_data
#undef sdk_data_count
const SdkFn (*player_sdk_fns_p)[];
const int *player_sdk_fn_count_p;
const char *const (*player_sdk_host_names_p)[];
const int *player_sdk_host_name_count_p;
const uint64_t (*player_sdk_ctors_p)[];
const int *player_sdk_ctor_count_p;
const SdkData (*player_sdk_data_p)[];
const int *player_sdk_data_count_p;
const char *const *player_sdk_install_names;
int player_sdk_install_count;
void strikers_bind_game_core(void);
static void fail(void){
    fputs("The local SDK build is missing or incompatible. Run setup again.\n",stderr);
    ExitProcess(5);
}
void strikers_bind_game(void){
    strikers_bind_game_core();
    wchar_t path[32768];
    DWORD n=GetModuleFileNameW(NULL,path,32768);
    if(!n||n>=32768)fail();
    wchar_t *slash=wcsrchr(path,L'\\');
    if(!slash||slash-path>32700)fail();
    wcscpy_s(slash+1,32768-(slash+1-path),L"strikers_sdk.dll");
    HMODULE module=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)fail();
    int (*bind)(const PlayerSdkHost *,PlayerSdkTables *)=(void *)GetProcAddress(module,"player_sdk_bind");
    PlayerSdkHost host={PLAYER_SDK_ABI,sizeof(PlayerSdkHost),call_indirect,host_import,guest_trap,
                       guest_unimpl,guest_svc,stx_diag,sdk_host_call,sdk_trap_call,sdk_dispatch};
    PlayerSdkTables t={0};t.size=sizeof t;
    if(!bind||!bind(&host,&t)||t.abi!=PLAYER_SDK_ABI||!t.functions||!t.function_count||*t.function_count<=0||
       !t.host_names||!t.host_name_count||!t.ctors||!t.ctor_count||!t.install_names||t.install_count<1)fail();
    player_sdk_fns_p=(const SdkFn (*)[])t.functions;player_sdk_fn_count_p=t.function_count;
    player_sdk_host_names_p=(const char *const (*)[])t.host_names;player_sdk_host_name_count_p=t.host_name_count;
    player_sdk_ctors_p=(const uint64_t (*)[])t.ctors;player_sdk_ctor_count_p=t.ctor_count;
    player_sdk_data_p=(const SdkData (*)[])t.data;player_sdk_data_count_p=t.data_count;
    player_sdk_install_names=t.install_names;player_sdk_install_count=t.install_count;
    printf("Local SDK bridge: %d functions; ABI %u\n",*t.function_count,t.abi);
}
