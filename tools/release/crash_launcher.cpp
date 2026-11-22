// SPDX-License-Identifier: GPL-3.0-or-later
// Local-only crash monitor. Windows debugger delivery preserves the crashing
// thread context without modifying the protected runtime or installing a service.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include "gui_self_test.h"
#include "../../runtime/core/report_send.h"

static HWND launcher_, launch_status_, play_, update_;
static bool running_, done_, started_;
static int game_exit_, argc_;
static wchar_t **argv_;
static std::wstring duration_, test_elf_, test_romfs_;
static std::wstring game_log_;
static bool graphics_ready_;
static ULONGLONG launch_start_;
static std::streamoff log_offset_;
static std::string log_pending_;
static std::wstring own_folder_;

// In-game updates (installer/update.py). The check runs before the game starts and gives up after 6 s: offline, or no
// service in strikers.ini, the game simply starts. Its one line of JSON is read for "available", "pending", "version"
// and "minutes"; everything else, signatures and file checks included, is update.py's.
struct UpdateOffer { bool available=false, pending=false; std::wstring version; int minutes=0; };
static std::string json_field(const std::string &s,const char *key){
    auto at=s.find(std::string("\"")+key+"\"");if(at==std::string::npos)return "";
    at=s.find(':',at);if(at==std::string::npos)return "";at++;
    while(at<s.size()&&s[at]==' ')at++;
    if(at<s.size()&&s[at]=='"'){auto end=s.find('"',at+1);return end==std::string::npos?"":s.substr(at+1,end-at-1);}
    auto end=s.find_first_of(",}",at);return s.substr(at,end==std::string::npos?std::string::npos:end-at);
}
static DWORD WINAPI update_check(void*){
    auto *offer=new UpdateOffer;
    std::wstring python=own_folder_+L"\\toolchain\\python\\python.exe",script=own_folder_+L"\\installer\\update.py";
    if(GetFileAttributesW(python.c_str())!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(script.c_str())!=INVALID_FILE_ATTRIBUTES){
        SECURITY_ATTRIBUTES sa={sizeof sa,nullptr,TRUE};HANDLE read=nullptr,write=nullptr;
        if(CreatePipe(&read,&write,&sa,0)){
            SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
            STARTUPINFOW si={sizeof si};si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=si.hStdError=write;
            PROCESS_INFORMATION p={};std::wstring command=L"\""+python+L"\" -I -X utf8 -B installer\\update.py check";
            BOOL ok=CreateProcessW(python.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,own_folder_.c_str(),&si,&p);
            CloseHandle(write);
            if(ok){
                if(WaitForSingleObject(p.hProcess,30000)!=WAIT_OBJECT_0)TerminateProcess(p.hProcess,1);      // beside the game now: it delays nothing
                // Only what is there once the check has ended: the game, started at the same moment, can inherit this pipe's
                // write end too, and a read to the end of the pipe would then wait until the game closes.
                std::string out;char buffer[4096];DWORD n,avail=0;
                while(out.size()<65536&&PeekNamedPipe(read,nullptr,0,nullptr,&avail,nullptr)&&avail&&
                      ReadFile(read,buffer,avail<sizeof buffer?avail:(DWORD)sizeof buffer,&n,nullptr)&&n)out.append(buffer,n);
                offer->available=json_field(out,"available")=="true";offer->pending=json_field(out,"pending")=="true";
                std::string v=json_field(out,"version");offer->version.assign(v.begin(),v.end());
                offer->minutes=atoi(json_field(out,"minutes").c_str());
                {   // what the check said, for a report: installer\update\last-check.txt
                    std::wstring folder=own_folder_+L"\\installer\\update";CreateDirectoryW(folder.c_str(),nullptr);
                    HANDLE f=CreateFileW((folder+L"\\last-check.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
                    if(f!=INVALID_HANDLE_VALUE){DWORD w;WriteFile(f,out.data(),(DWORD)out.size(),&w,nullptr);CloseHandle(f);}
                }
                CloseHandle(p.hThread);CloseHandle(p.hProcess);
            }
            CloseHandle(read);
        }
    }
    PostMessageW(launcher_,WM_APP+3,0,(LPARAM)offer);return 0;
}

// Observe the host's real first-present event. Process creation is not graphics
// readiness: a cold driver cache can spend minutes compiling before frame 1.
static void graphics_tick(HWND h) {
    if(!running_)return;
    std::ifstream in(game_log_,std::ios::binary);
    if(in){
        in.seekg(log_offset_);char chunk[16384];
        while(in.read(chunk,sizeof chunk)||in.gcount()){
            auto count=in.gcount();log_offset_+=count;log_pending_.append(chunk,(size_t)count);
            size_t end;
            while((end=log_pending_.find('\n'))!=std::string::npos){
                auto line=log_pending_.substr(0,end);log_pending_.erase(0,end+1);
                if(line.find("nvn_vk_frame: 1 draws ")!=std::string::npos ||
                   line.find("nvn_gl: frame ")!=std::string::npos)graphics_ready_=true;
            }
            if(log_pending_.size()>65536)log_pending_.clear();
        }
    }
    if(graphics_ready_){
        SetWindowTextW(launch_status_,L"The game is running; its log stays in userdata.");
        KillTimer(h,22);if(!gui_test::enabled)ShowWindow(h,SW_HIDE);
    }else{
        unsigned seconds=(unsigned)((GetTickCount64()-launch_start_)/1000);
        std::wstring text=L"Preparing graphics...\n"+std::to_wstring(seconds/60)+L"m "+std::to_wstring(seconds%60)+L"s elapsed.";
        SetWindowTextW(launch_status_,text.c_str());
    }
}

static std::wstring quote(const std::wstring &s) {
    std::wstring r=L"\""; unsigned slash=0;
    for(wchar_t c:s) {
        if(c==L'\\') { slash++; continue; }
        if(c==L'\"') { r.append(slash*2+1,L'\\'); r+=c; slash=0; continue; }
        r.append(slash,L'\\'); slash=0; r+=c;
    }
    r.append(slash*2,L'\\'); return r+L"\"";
}

static bool save_crash(const PROCESS_INFORMATION &p, DWORD tid, const EXCEPTION_RECORD &record,
                       const std::wstring &user, const std::wstring &log, std::wstring &base) {
    std::wstring folder=user+L"\\crashes";
    if(!CreateDirectoryW(folder.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) return false;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[100]; swprintf_s(name,L"\\%04u%02u%02u-%02u%02u%02u-%lu",t.wYear,t.wMonth,t.wDay,
                                t.wHour,t.wMinute,t.wSecond,p.dwProcessId);
    base=folder+name;
    HANDLE file=CreateFileW((base+L".dmp").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,tid);
    CONTEXT context={}; context.ContextFlags=CONTEXT_ALL;
    BOOL ok=FALSE;
    if(file!=INVALID_HANDLE_VALUE && thread && GetThreadContext(thread,&context)) {
        EXCEPTION_RECORD copy=record; EXCEPTION_POINTERS pointers={&copy,&context};
        MINIDUMP_EXCEPTION_INFORMATION info={tid,&pointers,FALSE};
        ok=MiniDumpWriteDump(p.hProcess,p.dwProcessId,file,MiniDumpNormal,&info,nullptr,nullptr);
    }
    if(thread) CloseHandle(thread);
    if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    std::ifstream input(log,std::ios::binary);
    std::ofstream output(base+L".txt",std::ios::binary);
    output<<"Crash report; sent only if the player says Yes. Exception 0x"<<std::hex<<record.ExceptionCode
          <<" at "<<record.ExceptionAddress<<"; minidump "<<(ok?"saved":"FAILED")<<"\r\n";
    // A bounded tail avoids copying multi-gigabyte logs into every crash report.
    input.seekg(0,std::ios::end); auto size=input.tellg();
    if(size>0) { input.seekg(size>65536?size-std::streamoff(65536):std::streamoff(0)); output<<input.rdbuf(); }
    return ok!=FALSE;
}

static std::string narrow(const std::wstring &w,UINT page=CP_UTF8) {
    if(w.empty())return "";
    int n=WideCharToMultiByte(page,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,'\0');WideCharToMultiByte(page,0,w.c_str(),(int)w.size(),s.data(),n,nullptr,nullptr);return s;
}
static std::wstring widen(const char *s) {
    int n=MultiByteToWideChar(CP_UTF8,0,s,-1,nullptr,0);if(n<=1)return L"";
    std::wstring w(n-1,L'\0');MultiByteToWideChar(CP_UTF8,0,s,-1,w.data(),n);return w;
}
// After a crash, on Yes: the crash's .txt and .dmp, the log and strikers.ini (private values out, user and PC names out of
// every text file) zipped into userdata\reports, then sent to the developer's report service (runtime/core/report_send.c).
// Returns what to tell the player.
static std::wstring send_crash_report(const std::wstring &own,const std::wstring &user,const std::wstring &base,const std::wstring &ini) {
    (void)own;
    SYSTEMTIME t;GetLocalTime(&t);wchar_t name[64];
    swprintf_s(name,L"crash-%04u%02u%02u-%02u%02u%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    std::wstring reports=user+L"\\reports",dir=reports+L"\\"+name,zip=dir+L".zip";
    CreateDirectoryW(reports.c_str(),nullptr);CreateDirectoryW(dir.c_str(),nullptr);
    std::vector<std::wstring> texts;
    if(!base.empty()) {
        std::wstring leaf=base.substr(base.find_last_of(L"\\/")+1);
        if(CopyFileW((base+L".txt").c_str(),(dir+L"\\"+leaf+L".txt").c_str(),FALSE))texts.push_back(dir+L"\\"+leaf+L".txt");
        CopyFileW((base+L".dmp").c_str(),(dir+L"\\"+leaf+L".dmp").c_str(),FALSE);
    }
    if(CopyFileW((user+L"\\last-game.log").c_str(),(dir+L"\\last-game.log").c_str(),FALSE))texts.push_back(dir+L"\\last-game.log");
    if(report_copy_settings(narrow(ini,CP_ACP).c_str(),narrow(dir+L"\\strikers.ini",CP_ACP).c_str()))texts.push_back(dir+L"\\strikers.ini");
    {std::ofstream json(dir+L"\\report.json",std::ios::binary);
     char made[32];sprintf_s(made,"%04u-%02u-%02uT%02u:%02u:%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
     json<<"{\"kind\":\"crash\",\"made\":\""<<made<<"\",\"description\":\"\"}\r\n";}
    for(const auto &text:texts)report_redact_file(narrow(text,CP_ACP).c_str(),16u<<20);
    ReportInfo info={};report_info_from_log(narrow(dir+L"\\last-game.log",CP_ACP).c_str(),&info);
    strcpy_s(info.kind,"crash");
    // One file: Windows' own tar writes zip archives (-a picks the format from the name), as F9's report does.
    wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);
    std::wstring tar=std::wstring(system)+L"\\tar.exe",cmd=quote(tar)+L" -a -c -f "+quote(std::wstring(name)+L".zip")+L" "+quote(name);
    std::vector<wchar_t> line(cmd.begin(),cmd.end());line.push_back(0);
    STARTUPINFOW si={sizeof(si)};PROCESS_INFORMATION p={};DWORD code=1;
    if(CreateProcessW(tar.c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,reports.c_str(),&si,&p)) {
        WaitForSingleObject(p.hProcess,60000);GetExitCodeProcess(p.hProcess,&code);CloseHandle(p.hThread);CloseHandle(p.hProcess);
    }
    if(code!=0||GetFileAttributesW(zip.c_str())==INVALID_FILE_ATTRIBUTES)
        return L"The crash report could not be packed into one file. It stays in userdata\\crashes; you can attach it to a report on RecompLauncher or GitHub.";
    WIN32_FIND_DATAW d;HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&d);
    if(h!=INVALID_HANDLE_VALUE){do{if(!(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))DeleteFileW((dir+L"\\"+d.cFileName).c_str());}while(FindNextFileW(h,&d));FindClose(h);}
    RemoveDirectoryW(dir.c_str());
    char url[512],id[32],why[300];report_url(narrow(ini,CP_ACP).c_str(),url,sizeof url);
    if(!url[0])return L"Sending reports is turned off in strikers.ini. The crash report stays in userdata\\crashes.";
    if(report_send(url,narrow(zip,CP_ACP).c_str(),&info,id,sizeof id,why,sizeof why))
        return L"Sent. Report ID "+widen(id)+L". Thank you! Mention it if you post about the crash on RecompLauncher or GitHub.";
    return L"The crash report could not be sent: "+widen(why)+L"\n\nIt was saved as userdata\\reports\\"+std::wstring(name)+
           L".zip; you can attach that file to a report on RecompLauncher or GitHub.";
}

static int run_game(int argc,wchar_t **argv) {
    wchar_t module[32768]; GetModuleFileNameW(nullptr,module,32768);
    std::wstring own=module; auto cut=own.find_last_of(L"\\/"); own=own.substr(0,cut);
    bool test=argc>1 && wcscmp(argv[1],L"--crash-test")==0;
    std::wstring user=own+L"\\userdata", exe=own+L"\\strikers_native.exe";
    if(test) {
        if(argc!=4) return 64;
        exe=argv[2]; user=argv[3];
        if(user.find(L"..")!=std::wstring::npos) return 64;
    }
    CreateDirectoryW(user.c_str(),nullptr);
    std::wstring log=user+L"\\last-game.log";
    // The run before stays as previous-game.log: a player who reopens the game before sending a report keeps it.
    MoveFileExW(log.c_str(),(user+L"\\previous-game.log").c_str(),MOVEFILE_REPLACE_EXISTING);
    SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE};
    HANDLE out=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,0,nullptr);
    if(out==INVALID_HANDLE_VALUE) return 65;
    auto log_failure=[&](const char *message){DWORD written;WriteFile(out,message,(DWORD)strlen(message),&written,nullptr);};
    if(!test&&!GetEnvironmentVariableW(L"STRIKERS_PAD_SELFTEST",nullptr,0)){
        std::wstring elf=gui_test::enabled&&!test_elf_.empty()?test_elf_:own+L"\\build\\dump\\update\\elf\\main.elf";
        for(const auto &file:{exe,own+L"\\strikers_game.dll",own+L"\\strikers_sdk.dll",elf}){
            if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES){
                log_failure("Run StrikersSetup.exe to finish building this PC copy first.\r\n");CloseHandle(out);return 70;
            }
        }
    }
    STARTUPINFOW si={sizeof(si)}; si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=si.hStdError=out; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION p={};
    SetEnvironmentVariableW(L"STRIKERS_USERDATA",user.c_str());
    std::wstring ini=own+L"\\strikers.ini", pads=user+L"\\controllers.ini";
    SetEnvironmentVariableW(L"STRIKERS_PC_CONFIG",ini.c_str());
    SetEnvironmentVariableW(L"STRIKERS_CONFIG",pads.c_str());
    std::wstring rom=own+L"\\build\\dump\\update\\romfs";
    if(gui_test::enabled&&!test_romfs_.empty())rom=test_romfs_;
    SetEnvironmentVariableW(L"STRIKERS_ROMFS",rom.c_str());
    // App-local, independent of the shortcut's launch directory. Never use the host's ../cache default here.
    if(!test){
        SetEnvironmentVariableW(L"STRIKERS_VK_CACHE",L"cache/vk");
        if(!GetEnvironmentVariableW(L"STRIKERS_VK_FRAMELOG",nullptr,0))
            SetEnvironmentVariableW(L"STRIKERS_VK_FRAMELOG",L"1");
    }
    // A process created under a debugger gets the Windows debug heap (NtGlobalFlag 0x70: no low-fragmentation heap, one lock,
    // tail and free checks on every block). The driver's shader compiler allocates per instruction: 16 pipeline workers took
    // 3.3 s per stage library instead of 55 ms and a cold first launch 48 minutes instead of 25 s (docs/audit-07.md).
    SetEnvironmentVariableW(L"_NO_DEBUG_HEAP",L"1");
    std::wstring cmd=quote(exe);
    if(!test) {
        cmd+=L" "+quote(gui_test::enabled&&!test_elf_.empty()?test_elf_:own+L"\\build\\dump\\update\\elf\\main.elf");
        if(gui_test::enabled)cmd+=L" "+duration_;
        else for(int i=1;i<argc;i++) cmd+=L" "+quote(argv[i]);
    }
    std::vector<wchar_t> buffer(cmd.begin(),cmd.end()); buffer.push_back(0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(!CreateProcessW(exe.c_str(),buffer.data(),nullptr,nullptr,TRUE,DEBUG_ONLY_THIS_PROCESS|CREATE_NO_WINDOW,
                       nullptr,own.c_str(),&si,&p)) {
        auto failure="The game process could not start (Windows error "+std::to_string(GetLastError())+").\r\n";
        log_failure(failure.c_str());CloseHandle(out);return 66;
    }
    // The game must outlive this process. By default Windows kills a debuggee when its debugger exits, and a Windows component
    // failing inside this launcher (exit 0xE0464645, seen under Chrome Remote Desktop) ended a friend's match mid-game.
    DebugSetProcessKillOnExit(FALSE);
    if(launcher_)PostMessageW(launcher_,WM_APP+1,0,0);
    if(gui_test::enabled){std::ofstream state(gui_test::folder.parent_path()/L"launcher-child.json");state<<"{\"pid\":"<<p.dwProcessId<<"}";}
    DEBUG_EVENT event={}; bool initial=true, dumped=false; DWORD exitcode=67; std::wstring crash_base;
    while(WaitForDebugEvent(&event,INFINITE)) {
        DWORD disposition=DBG_CONTINUE;
        if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
            auto &e=event.u.Exception;
            if(initial && e.ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT) initial=false;
            else {
                disposition=DBG_EXCEPTION_NOT_HANDLED;
                if(!e.dwFirstChance && !dumped) {
                    dumped=true; FlushFileBuffers(out);
                    save_crash(p,event.dwThreadId,e.ExceptionRecord,user,log,crash_base);
                }
            }
        } else if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
            if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            // These debug-event handles are distinct from PROCESS_INFORMATION handles.
            CloseHandle(event.u.CreateProcessInfo.hProcess); CloseHandle(event.u.CreateProcessInfo.hThread);
        } else if(event.dwDebugEventCode==CREATE_THREAD_DEBUG_EVENT) CloseHandle(event.u.CreateThread.hThread);
        else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        else if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            exitcode=event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition); break;
        }
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition);
    }
    CloseHandle(out); CloseHandle(p.hThread); CloseHandle(p.hProcess);
    // Asked every time, and nothing leaves the PC unless the answer is Yes. STRIKERS_REPORT_TEST_ANSWER=yes|no answers for a test
    // run (--crash-test), and the result is written to <userdata>\report-result.txt instead of a message box.
    wchar_t answer[8]={0}; bool scripted=GetEnvironmentVariableW(L"STRIKERS_REPORT_TEST_ANSWER",answer,8)>0;
    if(dumped && (scripted || (!test && !gui_test::enabled))) {
        bool yes=scripted?!_wcsicmp(answer,L"yes"):MessageBoxW(nullptr,
            L"The game closed unexpectedly.\n\nSend the crash report to the developer? It contains the game's log, a memory snapshot of "
            L"the game when it crashed, and your PC's processor, graphics card, Windows version and memory. Your Windows user name "
            L"and PC name are taken out of file paths.\n\nA copy stays in userdata\\crashes either way.",
            L"Strikers Recomp",MB_YESNO|MB_ICONERROR|MB_DEFBUTTON1)==IDYES;
        std::wstring result=yes?send_crash_report(own,user,crash_base,ini):L"";
        if(scripted){std::ofstream(user+L"\\report-result.txt",std::ios::binary)<<narrow(yes?result:L"not asked to send");}
        else if(yes)MessageBoxW(nullptr,result.c_str(),L"Strikers Recomp",MB_OK|MB_ICONINFORMATION);
    }
    return (int)exitcode;
}

static DWORD WINAPI launch_worker(void*) {
    game_exit_=run_game(argc_,argv_);PostMessageW(launcher_,WM_APP+2,game_exit_,0);return 0;
}
static LRESULT CALLBACK launch_messages(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_CREATE){
        launcher_=h;
        launch_status_=CreateWindowW(L"STATIC",L"Ready to launch your locally built game.",WS_VISIBLE|WS_CHILD,20,20,480,52,h,nullptr,nullptr,nullptr);
        play_=CreateWindowW(L"BUTTON",L"Play",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,370,85,110,32,h,(HMENU)1,nullptr,nullptr);
        update_=CreateWindowW(L"BUTTON",L"Update",WS_CHILD|BS_PUSHBUTTON,250,85,110,32,h,(HMENU)2,nullptr,nullptr);return 0;
    }
    if(msg==WM_COMMAND&&wp==1&&!running_){
        running_=true;done_=started_=graphics_ready_=false;EnableWindow(play_,FALSE);ShowWindow(update_,SW_HIDE);
        launch_start_=GetTickCount64();log_offset_=0;log_pending_.clear();
        SetTimer(h,22,500,nullptr);
        SetWindowTextW(launch_status_,L"Starting Strikers Recomp…");
        HANDLE worker=CreateThread(nullptr,0,launch_worker,nullptr,0,nullptr);
        if(!worker){running_=false;done_=true;game_exit_=68;SetWindowTextW(launch_status_,L"The game could not start; try again.");}
        else CloseHandle(worker);return 0;
    }
    if(msg==WM_APP+3){
        /* The check ran while the game started. A newer version is told to the game through installer\update\available.json:
         * its start screen and PC menu offer UPDATE, which leaves installer\update\requested and closes the game (WM_APP+2). */
        auto *offer=(UpdateOffer*)lp;
        std::wstring folder=own_folder_+L"\\installer\\update",note=folder+L"\\available.json";
        if(offer->available&&!offer->pending){
            CreateDirectoryW(folder.c_str(),nullptr);
            std::string v(offer->version.begin(),offer->version.end());
            std::string json="{\"version\":\""+v+"\",\"minutes\":"+std::to_string(offer->minutes>0?offer->minutes:1)+"}";
            HANDLE f=CreateFileW(note.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
            if(f!=INVALID_HANDLE_VALUE){DWORD n;WriteFile(f,json.data(),(DWORD)json.size(),&n,nullptr);CloseHandle(f);}
        }else DeleteFileW(note.c_str());
        delete offer;return 0;
    }
    if(msg==WM_COMMAND&&wp==2&&!running_){
        std::wstring setup=own_folder_+L"\\StrikersSetup.exe",command=L"\""+setup+L"\" --update";
        STARTUPINFOW si={sizeof si};PROCESS_INFORMATION p={};
        if(CreateProcessW(setup.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,own_folder_.c_str(),&si,&p)){
            CloseHandle(p.hThread);CloseHandle(p.hProcess);DestroyWindow(h);
        }else SetWindowTextW(launch_status_,L"The update could not start; run StrikersSetup.exe --update from this folder.");
        return 0;
    }
    if(msg==WM_APP+1){started_=true;graphics_tick(h);return 0;}
    if(msg==WM_TIMER&&wp==22){graphics_tick(h);return 0;}
    if(msg==WM_APP+2){running_=false;done_=true;EnableWindow(play_,TRUE);
        KillTimer(h,22);
        const wchar_t *message=wp==71?L"Your game files were moved, renamed or changed. Run StrikersSetup.exe in this folder and choose them again.":
            wp==70?L"Run StrikersSetup.exe to finish building this PC copy first.":wp==65?L"The game log could not be created in this folder.":wp?L"The game closed with an error; see userdata\\last-game.log.":L"Game closed successfully.";
        SetWindowTextW(launch_status_,message);
        // Self-test snapshots run on the next script timer after repaint. Audit 09: a failure stays on screen. This window is
        // the only place a player learns why the game did not start, and it may be hidden since the first frame.
        if(!gui_test::enabled){
            std::wstring requested=own_folder_+L"\\installer\\update\\requested";
            if(GetFileAttributesW(requested.c_str())!=INVALID_FILE_ATTRIBUTES){DeleteFileW(requested.c_str());SendMessageW(h,WM_COMMAND,2,0);return 0;}
            // Settings > Saves: a save brought in from an emulator is put in place when the game starts again.
            std::wstring restart=own_folder_+L"\\userdata\\restart-requested";
            if(!wp&&GetFileAttributesW(restart.c_str())!=INVALID_FILE_ATTRIBUTES){DeleteFileW(restart.c_str());SendMessageW(h,WM_COMMAND,1,0);return 0;}
            if(wp)ShowWindow(h,SW_SHOW);else DestroyWindow(h);
        }return 0;}
    if(msg==WM_TIMER&&gui_test::enabled){
        using namespace gui_test;
        try{
            if(cursor>=lines.size())return 0;
            auto line=lines[cursor];auto at=line.find(L' ');auto verb=line.substr(0,at);
            auto arg=at==std::wstring::npos?L"":line.substr(at+1);bool advance=true;
            if(verb==L"snapshot")snapshot(h,arg);
            else if(verb==L"duration")duration_=std::to_wstring(std::stoi(arg));
            else if(verb==L"elf")test_elf_=arg;
            else if(verb==L"romfs")test_romfs_=arg;
            else if(verb==L"click")SendMessageW(h,WM_COMMAND,1,0);
            else if(verb==L"wait"){
                if(!deadline)deadline=GetTickCount64()+60ULL*60*1000;
                bool ready=arg==L"started"?started_:arg==L"graphics"?graphics_ready_:done_;
                if(done_&&!ready)throw std::runtime_error("Game did not start");
                if(!ready){advance=false;if(GetTickCount64()>deadline)throw std::runtime_error("Launcher self-test timeout");}
            }else if(verb==L"expect"){
                if((arg==L"success")!=(game_exit_==0))throw std::runtime_error("Unexpected game exit");
            }else if(verb==L"exit"){
                if(running_)throw std::runtime_error("Game still running");record(L"PASS");DestroyWindow(h);return 0;
            }else throw std::runtime_error("Unknown launcher self-test action");
            if(advance){record(verb);cursor++;deadline=0;}
        }catch(...){record(L"FAIL");if(!running_){DestroyWindow(h);PostQuitMessage(3);}}
        return 0;
    }
    if(msg==WM_CLOSE&&running_)return 0;
    if(msg==WM_DESTROY){PostQuitMessage(gui_test::enabled?0:game_exit_);return 0;}
    return DefWindowProcW(h,msg,wp,lp);
}
int wmain(int argc,wchar_t **argv){
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Existing crash regression mode keeps exactly the same debugger path.
    if(argc>1&&!wcscmp(argv[1],L"--crash-test"))return run_game(argc,argv);
    argc_=argc;argv_=argv;
    wchar_t module[32768];GetModuleFileNameW(nullptr,module,32768);
    own_folder_=std::filesystem::path(module).parent_path().wstring();
    game_log_=own_folder_+L"\\userdata\\last-game.log";
    try{if(argc==3&&!wcscmp(argv[1],L"--self-test"))gui_test::load(argv[2]);}
    catch(...){return 64;}
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSEXW cls={sizeof cls};cls.lpfnWndProc=launch_messages;cls.hInstance=instance;cls.lpszClassName=L"StrikersPlayerLauncher";
    // The project's own icon (resource 1, tools/release/app_icon.py); the game's icon is for the game's shortcut and window.
    cls.hIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE);
    cls.hIconSm=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0);
    cls.hCursor=LoadCursor(nullptr,IDC_ARROW);cls.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassExW(&cls);
    HWND h=CreateWindowW(cls.lpszClassName,L"Strikers Recomp",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,520,170,nullptr,nullptr,instance,nullptr);
    if(!h)return 69;
    /* As a PC game starts: the game window is the first thing on screen. This window stays hidden; it shows itself only
     * when the game cannot start or ends with an error, the one place a player learns why. The self-tests keep it shown. */
    if(gui_test::enabled){ShowWindow(h,SW_SHOW);SetTimer(h,21,200,nullptr);}
    else{
        std::wstring pending=own_folder_+L"\\installer\\update\\pending.json",level=own_folder_+L"\\build\\cpu-level.txt";
        char built[8]={0};
        HANDLE lf=CreateFileW(level.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(lf!=INVALID_HANDLE_VALUE){DWORD n;ReadFile(lf,built,7,&n,nullptr);CloseHandle(lf);}
        bool wrong_cpu=(!strncmp(built,"v3",2)&&!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))||
                       (!strncmp(built,"v2",2)&&!IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE));
        if(GetFileAttributesW(pending.c_str())!=INVALID_FILE_ATTRIBUTES||wrong_cpu){
            // an update that has to be finished, or a copy built for a newer processor: the game cannot run as it is
            SendMessageW(h,WM_COMMAND,2,0);
        }else{
            PostMessageW(h,WM_COMMAND,1,0);
            if(argc==1){HANDLE check=CreateThread(nullptr,0,update_check,nullptr,0,nullptr);if(check)CloseHandle(check);}
        }
    }
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return (int)msg.wParam;
}
