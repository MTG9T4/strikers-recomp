// SPDX-License-Identifier: GPL-3.0-or-later
// A /MT process has no preloaded VC DLLs. Validate the release's app-local DLLs
// in a fresh process rather than accidentally reusing Python's loaded runtime.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
int wmain(){
    wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);
    wchar_t *slash=wcsrchr(path,L'\\');if(!slash)return 1;slash[1]=0;
    const wchar_t *libraries[]={L"SDL3.dll",L"strikers_glslang.dll",L"libopus-0.dll",L"strikers_zt.dll"};
    for(const auto *name:libraries){wchar_t dll[32768];swprintf_s(dll,L"%s%s",path,name);
        if(!LoadLibraryExW(dll,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)){
            fwprintf(stderr,L"FAIL loading %s: %lu\n",name,GetLastError());return 2;}}
    const wchar_t *runtimes[]={L"msvcp140.dll",L"vcruntime140.dll",L"vcruntime140_1.dll"};
    for(const auto *name:runtimes){HMODULE module=GetModuleHandleW(name);wchar_t file[32768];
        if(!module||!GetModuleFileNameW(module,file,32768)||_wcsnicmp(file,path,wcslen(path))){
            fwprintf(stderr,L"FAIL private runtime: %s\n",name);return 3;}
        wprintf(L"App-local runtime: %s\n",file);}
    puts("PASS: all prebuilt DLLs load with the private runtime; no runtime installer");return 0;
}
