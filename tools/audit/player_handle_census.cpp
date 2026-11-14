// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only handle-type census of one explicitly identified test process.
// PSS captures handles only: no process clone or memory dump. Emits no object names.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processsnapshot.h>
#include <map>
#include <string>
#include <iostream>

int wmain(int argc,wchar_t **argv) {
    if(argc!=3) return 64;
    DWORD pid=wcstoul(argv[1],nullptr,10);
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE,FALSE,pid);
    if(!process) return 65;
    wchar_t image[32768]; DWORD size=32768;
    if(!QueryFullProcessImageNameW(process,0,image,&size)||_wcsicmp(image,argv[2])) {
        CloseHandle(process); return 66;
    }
    FILETIME created,exited,kernel,user;
    if(!GetProcessTimes(process,&created,&exited,&kernel,&user)) {CloseHandle(process);return 67;}
    HPSS snapshot=nullptr; HPSSWALK walk=nullptr;
    auto flags=PSS_CAPTURE_HANDLES|PSS_CAPTURE_HANDLE_NAME_INFORMATION|
               PSS_CAPTURE_HANDLE_BASIC_INFORMATION|PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION;
    DWORD error=PssCaptureSnapshot(process,flags,0,&snapshot);
    std::map<std::wstring,unsigned> types;
    unsigned retired=0,total=0;
    if(!error) error=PssWalkMarkerCreate(nullptr,&walk);
    if(!error) {
        PSS_HANDLE_ENTRY entry={};
        while(!(error=PssWalkSnapshot(snapshot,PSS_WALK_HANDLES,walk,&entry,sizeof entry))) {
            std::wstring type=entry.TypeName?std::wstring(entry.TypeName,entry.TypeNameLength/sizeof(wchar_t)):L"unknown";
            while(!type.empty()&&type.back()==L'\0') type.pop_back();
            types[type]++; total++;
            if(entry.ObjectType==PSS_OBJECT_TYPE_THREAD && entry.TypeSpecificInformation.Thread.ExitStatus!=STILL_ACTIVE) retired++;
        }
        if(error==ERROR_NO_MORE_ITEMS) error=0;
    }
    if(walk) PssWalkMarkerFree(walk);
    if(snapshot) PssFreeSnapshot(GetCurrentProcess(),snapshot);
    CloseHandle(process);
    if(error) {std::cerr<<"PSS error "<<error<<"\n";return 68;}
    std::wcout<<L"{\"pid\":"<<pid<<L",\"created_filetime\":"
              <<((static_cast<unsigned long long>(created.dwHighDateTime)<<32)|created.dwLowDateTime)
              <<L",\"total\":"<<total<<L",\"retired_thread_handles\":"<<retired<<L",\"types\":{";
    bool comma=false;
    for(auto &item:types) {
        if(comma) std::wcout<<L","; comma=true;
        // Kernel object type names are identifiers; no object paths are emitted.
        for(auto c:item.first) if(!iswalnum(c)&&c!=L' ') return 69;
        std::wcout<<L"\""<<item.first<<L"\":"<<item.second;
    }
    std::wcout<<L"}}\n";
    return 0;
}
