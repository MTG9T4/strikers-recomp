// SPDX-License-Identifier: GPL-3.0-or-later
// Plain Win32 setup, statically linked CRT. Windows' built-in PowerShell expands
// the hash-verified Python ZIP; no runtime, service or system package is installed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cwctype>
#include <shellapi.h>
#include <shobjidl.h>
#include "../tools/release/gui_self_test.h"

static HWND window_, paths_[3], status_, progress_, start_;
static HFONT font_;
static bool active_;
static std::wstring root_, selected_[3];
// The game comes from the ExeFS and RomFS folders the player extracted from their own Switch (installer/game_files.py).
static bool folders_=true;
static int rows_=2;               // the chosen paths: ExeFS and RomFS folders
static bool pick_folder(HWND owner,std::wstring &out){
    IFileOpenDialog *dialog=nullptr;bool ok=false;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return false;
    DWORD options=0;dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT);
    IShellItem *item=nullptr;
    if(SUCCEEDED(dialog->Show(owner))&&SUCCEEDED(dialog->GetResult(&item))){
        PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){out=path;CoTaskMemFree(path);ok=true;}
        item->Release();
    }
    dialog->Release();return ok;
}
static HANDLE log_=INVALID_HANDLE_VALUE;
static HANDLE children_;
static std::wstring error_;
static int percent_;
static bool finished_, successful_, update_mode_;
static std::wstring injected_;
static constexpr UINT STATUS=WM_APP+1, FINISHED=WM_APP+2;
static const wchar_t *PYTHON_HASH=L"91d828c2da3a029b41699e918674a0cb379c02cf20dab9c501306885f837402a";

static std::wstring wide(const std::string &s){
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    std::wstring r(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;
}
static std::string utf8(const std::wstring &s){
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string r(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;
}
static std::wstring quote(const std::wstring &s){
    std::wstring r=L"\"";unsigned slashes=0;
    for(wchar_t c:s){
        if(c==L'\\'){slashes++;continue;}
        if(c==L'\"'){r.append(slashes*2+1,L'\\');r+=c;slashes=0;continue;}
        r.append(slashes,L'\\');slashes=0;r+=c;
    }
    r.append(slashes*2,L'\\');return r+L"\"";
}
static std::string json_string(const std::wstring &s){
    std::string r="\"";
    for(unsigned char c:utf8(s)){
        if(c=='\\'||c=='\"'){r+='\\';r+=(char)c;}
        else if(c<32)throw std::runtime_error("Control character in input path");
        else r+=(char)c;
    }
    return r+"\"";
}
static void directory(const std::wstring &path){
    if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Cannot create an installation subfolder");
    DWORD attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES||!(attrs&FILE_ATTRIBUTE_DIRECTORY)||(attrs&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Installation folders must not be junctions or symbolic links");
}
static void write(HANDLE file,const void *data,DWORD bytes){
    DWORD written=0;
    if(!WriteFile(file,data,bytes,&written,nullptr)||written!=bytes)throw std::runtime_error("Could not save a local file");
}
static void note(const std::string &s){if(log_!=INVALID_HANDLE_VALUE){write(log_,s.data(),(DWORD)s.size());write(log_,"\r\n",2);}}
static void update(int percent,const std::wstring &s){
    auto message=new std::wstring(s);
    if(!PostMessageW(window_,STATUS,percent,(LPARAM)message))delete message;
}
static std::wstring hash(const std::wstring &path){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE digest=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 unavailable");
    DWORD length=0,done=0;
    BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&length,sizeof length,&done,0);
    std::vector<unsigned char> object(length),buffer(1024*1024);unsigned char result[32];
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    bool ok=file!=INVALID_HANDLE_VALUE&&BCryptCreateHash(algorithm,&digest,object.data(),length,nullptr,0,0)>=0;
    while(ok){DWORD read=0;if(!ReadFile(file,buffer.data(),(DWORD)buffer.size(),&read,nullptr)){ok=false;break;}
        if(!read)break;if(BCryptHashData(digest,buffer.data(),read,0)<0){ok=false;break;}}
    if(ok)ok=BCryptFinishHash(digest,result,sizeof result,0)>=0;
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(digest)BCryptDestroyHash(digest);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok)throw std::runtime_error("Could not verify the Python archive");
    static const wchar_t hex[]=L"0123456789abcdef";std::wstring text;
    for(unsigned char c:result){text+=hex[c>>4];text+=hex[c&15];}return text;
}
static void download_python(const std::wstring &destination){
    HINTERNET session=WinHttpOpen(L"StrikersSetup/0.8",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session)throw std::runtime_error("Cannot start the tool download");
    WinHttpSetTimeouts(session,15000,30000,30000,60000);
    HINTERNET connection=WinHttpConnect(session,L"www.python.org",INTERNET_DEFAULT_HTTPS_PORT,0);
    HINTERNET request=connection?WinHttpOpenRequest(connection,L"GET",L"/ftp/python/3.13.9/python-3.13.9-embed-amd64.zip",
                                                   nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
    DWORD disabled=WINHTTP_DISABLE_REDIRECTS,revocation=WINHTTP_ENABLE_SSL_REVOCATION;
    if(request){WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof disabled);
                WinHttpSetOption(request,WINHTTP_OPTION_ENABLE_FEATURE,&revocation,sizeof revocation);}
    bool ok=request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr);
    DWORD status=0,size=sizeof status;
    if(ok)ok=WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr)&&status==200;
    HANDLE file=INVALID_HANDLE_VALUE;
    if(ok)file=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)ok=false;
    unsigned char buffer[65536];unsigned long long total=0;
    while(ok){DWORD read=0;if(!WinHttpReadData(request,buffer,sizeof buffer,&read)){ok=false;break;}
        if(!read)break;total+=read;if(total>20*1024*1024){ok=false;break;}
        DWORD written=0;if(!WriteFile(file,buffer,read,&written,nullptr)||written!=read){ok=false;break;}}
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(request)WinHttpCloseHandle(request);if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(session);
    if(!ok)throw std::runtime_error("The official Python download could not finish");
}
static void process(const std::wstring &exe,const std::wstring &args){
    SECURITY_ATTRIBUTES sa={sizeof sa,nullptr,TRUE};HANDLE read=nullptr,write_end=nullptr;
    if(!CreatePipe(&read,&write_end,&sa,0))throw std::runtime_error("Cannot start a build helper");
    SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof si};si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;
    si.hStdOutput=si.hStdError=write_end;si.hStdInput=nullptr;
    PROCESS_INFORMATION p={};std::wstring command=quote(exe)+L" "+args;
    bool started=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,root_.c_str(),&si,&p)!=FALSE;
    CloseHandle(write_end);
    if(!started){CloseHandle(read);throw std::runtime_error("A required Windows build helper is unavailable");}
    if(!AssignProcessToJobObject(children_,p.hProcess)){
        TerminateProcess(p.hProcess,1);CloseHandle(p.hThread);CloseHandle(p.hProcess);CloseHandle(read);
        throw std::runtime_error("Cannot isolate the build helper process");
    }
    ResumeThread(p.hThread);
    std::string pending;char buffer[4096];DWORD count;
    while(ReadFile(read,buffer,sizeof buffer,&count,nullptr)&&count){
        write(log_,buffer,count);pending.append(buffer,count);
        for(;;){auto end=pending.find('\n');if(end==std::string::npos)break;
            std::string line=pending.substr(0,end);pending.erase(0,end+1);
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(line.rfind("@error ",0)==0)error_=wide(line.substr(7));
            if(line.rfind("@phase8 ",0)==0){char *tail=nullptr;long percent=strtol(line.c_str()+8,&tail,10);
                if(tail&&*tail=='|')update((int)percent,wide(tail+1));}}
        if(pending.size()>1024*1024)pending.clear();
    }
    CloseHandle(read);WaitForSingleObject(p.hProcess,INFINITE);DWORD exit=1;GetExitCodeProcess(p.hProcess,&exit);
    CloseHandle(p.hThread);CloseHandle(p.hProcess);
    if(exit)throw std::runtime_error(error_.empty()?"A build helper failed; see installer/setup.log":utf8(error_));
}
static std::wstring ps_literal(std::wstring text){
    std::wstring result=L"'";for(wchar_t c:text){result+=c;if(c==L'\''||c==0x2018||c==0x2019||c==0x201a||c==0x201b)result+=c;}return result+L"'";
}
static std::wstring resolved(const std::wstring &path,bool folder){
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,folder?FILE_FLAG_BACKUP_SEMANTICS:0,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot read selected path");
    wchar_t result[32768];DWORD n=GetFinalPathNameByHandleW(file,result,32768,FILE_NAME_NORMALIZED);
    CloseHandle(file);if(!n||n>=32768)throw std::runtime_error("Cannot resolve selected path");return result;
}
static bool readonly_inputs(){
    std::wstring root=resolved(root_,true)+L"\\";
    for(int i=0;i<rows_;i++){const auto &file=selected_[i];
        if(file.empty()||GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)continue;
        std::wstring actual=resolved(file,folders_);
        if(folders_&&!_wcsnicmp(root.c_str(),(actual+L"\\").c_str(),actual.size()+1))return false;   // the folder holds this one
        if(_wcsnicmp(actual.c_str(),root.c_str(),root.size()))continue;
        std::wstring relative=actual.substr(root.size());
        std::transform(relative.begin(),relative.end(),relative.begin(),towlower);
        std::wstring first=relative.substr(0,relative.find(L'\\'));
        if(first==L"installer"||first==L"build"||first==L"toolchain"||first==L"userdata")return false;
        if(relative.find(L'\\')==std::wstring::npos){
            auto dot=relative.find_last_of(L'.');std::wstring extension=dot==std::wstring::npos?L"":relative.substr(dot);
            if(extension==L".exe"||extension==L".dll"||extension==L".lnk"||relative==L"strikers.ini"||relative==L"release-manifest.json")return false;
        }
    }
    return true;
}
static void sandbox_environment(){
    const std::wstring sandbox=root_+L"\\installer\\sandbox";
    directory(sandbox);directory(sandbox+L"\\temp");directory(sandbox+L"\\profile");
    directory(sandbox+L"\\profile\\local");directory(sandbox+L"\\profile\\roaming");
    SetEnvironmentVariableW(L"TEMP",(sandbox+L"\\temp").c_str());
    SetEnvironmentVariableW(L"TMP",(sandbox+L"\\temp").c_str());
    SetEnvironmentVariableW(L"HOME",sandbox.c_str());
    SetEnvironmentVariableW(L"USERPROFILE",sandbox.c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA",(sandbox+L"\\profile\\local").c_str());
    SetEnvironmentVariableW(L"APPDATA",(sandbox+L"\\profile\\roaming").c_str());
    SetEnvironmentVariableW(L"PSModuleAnalysisCachePath",(sandbox+L"\\powershell-analysis.cache").c_str());
}
// StrikersSetup.exe --update (started by the launcher): installer/update.py apply, with this window's progress bar.
// The player picks nothing; the files of the first setup are used again.
static DWORD WINAPI update_worker(void*){
    bool success=false;
    try{
        directory(root_+L"\\installer");
        log_=CreateFileW((root_+L"\\installer\\update.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
        if(log_==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot save update.log in this folder");
        std::wstring python=root_+L"\\toolchain\\python\\python.exe";
        if(GetFileAttributesW(python.c_str())==INVALID_FILE_ATTRIBUTES||GetFileAttributesW((root_+L"\\installer\\update.py").c_str())==INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("Finish setup first: choose your files and Check files and build");
        sandbox_environment();
        update(1,L"Checking for the update…");
        process(python,L"-I -X utf8 -B installer/update.py apply");success=true;
    }catch(const std::exception &e){error_=wide(e.what());if(error_.empty()||error_.back()!=L'.')error_+=L'.';try{note(utf8(error_));}catch(...){}}
    if(log_!=INVALID_HANDLE_VALUE){CloseHandle(log_);log_=INVALID_HANDLE_VALUE;}
    PostMessageW(window_,FINISHED,success,0);return 0;
}
static DWORD WINAPI build(void*){
    bool success=false;
    try{
        if(utf8(root_).size()>140)throw std::runtime_error("Move this folder to a shorter path, such as C:\\Games\\StrikersPC");
        directory(root_+L"\\installer");
        log_=CreateFileW((root_+L"\\installer\\setup.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
        if(log_==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot save setup.log in this folder");
        if(GetFileAttributesW((root_+L"\\release-manifest.json").c_str())==INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("No verified player-release manifest; this is a developer review build");
        // Keep PowerShell's first-run cache and every helper's temporary/profile
        // paths local too, before embedded Python can construct its own block.
        sandbox_environment();
        directory(root_+L"\\installer\\cache");directory(root_+L"\\toolchain");directory(root_+L"\\toolchain\\python");
        std::string request="{\"exefs\":"+json_string(selected_[0])+",\"romfs\":"+json_string(selected_[1])+"}";
        HANDLE file=CreateFileW((root_+L"\\installer\\request.json").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot save the local file selection");
        write(file,request.data(),(DWORD)request.size());CloseHandle(file);
        const std::wstring archive=root_+L"\\installer\\cache\\python-3.13.9-embed-amd64.zip";
        if(GetFileAttributesW(archive.c_str())==INVALID_FILE_ATTRIBUTES){
            if(GetEnvironmentVariableW(L"STRIKERS_OFFLINE",nullptr,0))throw std::runtime_error("A required cached build helper is missing; restore it and retry");
            update(1,L"Downloading the Python build helper (about 11 MB)…");
            download_python(archive+L".part");
            if(hash(archive+L".part")!=PYTHON_HASH)throw std::runtime_error("Python download failed its SHA256 check");
            if(!MoveFileExW((archive+L".part").c_str(),archive.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("Cannot save the verified Python download");
        }
        if(hash(archive)!=PYTHON_HASH)throw std::runtime_error("Python archive failed its SHA256 check");
        wchar_t system[32768];GetSystemDirectoryW(system,32768);
        std::wstring powershell=std::wstring(system)+L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        // Use the OS framework directly: Archive-module auto-loading depends
        // on profile/module discovery and failed in the isolated real run.
        std::wstring script=L"$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
            L"$zip=[IO.Compression.ZipFile]::OpenRead("+ps_literal(archive)+L"); try { foreach($entry in $zip.Entries) { "
            L"$dest=[IO.Path]::Combine("+ps_literal(root_+L"\\toolchain\\python")+L",$entry.FullName); "
            L"[IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$dest,$true) } } finally { $zip.Dispose() }";
        process(powershell,L"-NoLogo -NoProfile -NonInteractive -Command "+quote(script));
        std::wstring python=root_+L"\\toolchain\\python\\python.exe";
        process(python,L"-I -X utf8 -B installer/bootstrap.py");process(python,L"-I -X utf8 -B installer/player_setup.py");success=true;
    }catch(const std::exception &e){error_=wide(e.what());if(error_.empty()||error_.back()!=L'.')error_+=L'.';try{note(utf8(error_));}catch(...){}}
    if(log_!=INVALID_HANDLE_VALUE){CloseHandle(log_);log_=INVALID_HANDLE_VALUE;}
    PostMessageW(window_,FINISHED,success,0);return 0;
}
static HWND control(const wchar_t *kind,const wchar_t *text,int x,int y,int w,int h,DWORD style,int id=0){
    if(!wcscmp(kind,L"BUTTON")||!wcscmp(kind,L"EDIT"))style|=WS_TABSTOP;
    HWND c=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window_,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,(WPARAM)font_,TRUE);return c;
}
static void enable_buttons(bool value){for(int id=100;id<105;id++)EnableWindow(GetDlgItem(window_,id),value);}
static void self_tick(HWND h){
    using namespace gui_test;
    if(cursor>=lines.size())return;
    const auto line=lines[cursor];auto split=line.find(L' ');
    auto verb=line.substr(0,split),arg=split==std::wstring::npos?L"":line.substr(split+1);
    bool advance=true;
    if(verb==L"snapshot")snapshot(h,arg);
    else if(verb==L"select"){
        if(arg.size()<3||arg[1]!='|'||arg[0]<'0'||arg[0]>'2')throw std::runtime_error("Invalid select action");
        injected_=arg.substr(2);SendMessageW(h,WM_COMMAND,100+arg[0]-'0',0);
    }else if(verb==L"click")SendMessageW(h,WM_COMMAND,103,0);
    else if(verb==L"delay"){
        if(!deadline)deadline=GetTickCount64()+std::stoull(arg);
        advance=GetTickCount64()>=deadline;
    }else if(verb==L"wait"||verb==L"interrupt"){
        if(!deadline)deadline=GetTickCount64()+60ULL*60*1000;
        bool ready=arg==L"done"?finished_:percent_>=std::stoi(arg);
        if(finished_&&!ready)throw std::runtime_error("Setup finished before expected progress");
        if(!ready){if(GetTickCount64()>deadline)throw std::runtime_error("Self-test state timeout");advance=false;}
        else if(verb==L"interrupt"){record(L"Interrupted actual helper tree");TerminateJobObject(children_,1223);}
    }else if(verb==L"expect"){
        if((arg==L"success")!=successful_)throw std::runtime_error("Unexpected setup outcome");
    }else if(verb==L"exit"){
        if(active_)throw std::runtime_error("Cannot exit self-test with active build");
        record(L"PASS");DestroyWindow(h);return;
    }else throw std::runtime_error("Unknown self-test action");
    if(advance){record(verb);cursor++;deadline=0;}
}
static LRESULT CALLBACK messages(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==DM_GETDEFID)return MAKELRESULT(103,DC_HASDEFID);
    if(msg==WM_TIMER&&wp==21&&gui_test::enabled){
        try{self_tick(h);}catch(const std::exception &e){gui_test::record(L"FAIL");TerminateJobObject(children_,1);DestroyWindow(h);PostQuitMessage(3);}return 0;
    }
    if(msg==WM_CREATE){
        window_=h;font_=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        if(update_mode_){
            control(L"STATIC",L"Updating Strikers Recomp",24,20,650,30,0);
            control(L"STATIC",L"Your game files, saves and settings stay as they are. Keep the PC on;\nthe game starts again when the update is done.",24,58,650,50,0);
            status_=control(L"STATIC",L"Starting the update…",24,345,650,48,0);
            progress_=control(PROGRESS_CLASSW,L"",24,400,650,19,0);SendMessageW(progress_,PBM_SETRANGE32,0,100);
            active_=true;HANDLE worker=CreateThread(nullptr,0,update_worker,nullptr,0,nullptr);
            if(worker)CloseHandle(worker);else{active_=false;SetWindowTextW(status_,L"The update could not start; close this window and try again.");}
            return 0;
        }
        control(L"STATIC",L"Build your own PC copy",24,20,650,30,0);
        const wchar_t *intro=L"Choose the folders you extracted from your own Switch with update 1.3.2. They stay\nunchanged. Your game stays on this PC and is never uploaded.";
        const wchar_t *labels[3]={L"ExeFS folder — main, main.npdm, sdk… of update 1.3.2",L"RomFS folder — the game's data with update 1.3.2 applied",L""};
        control(L"STATIC",intro,24,58,650,50,0);
        for(int i=0;i<rows_;i++){
            control(L"STATIC",labels[i],24,124+i*65,650,24,0);
            paths_[i]=control(L"EDIT",L"",24,150+i*65,540,26,WS_BORDER|ES_AUTOHSCROLL|ES_READONLY);
            control(L"BUTTON",L"Choose…",575,148+i*65,100,29,BS_PUSHBUTTON,100+i);
        }
        status_=control(L"STATIC",L"Allow 10–60 minutes or longer and at least 20 GB free space.",24,345,650,48,0);
        progress_=control(PROGRESS_CLASSW,L"",24,400,650,19,0);SendMessageW(progress_,PBM_SETRANGE32,0,100);
        start_=control(L"BUTTON",L"Check files and build",445,438,230,32,BS_DEFPUSHBUTTON,103);
        control(L"STATIC",L"Everything is kept in this folder.",24,444,400,25,0);
        return 0;
    }
    if(msg==WM_COMMAND&&LOWORD(wp)>=100&&LOWORD(wp)<100+rows_){
        int i=LOWORD(wp)-100;std::wstring chosen;
        if(gui_test::enabled&&!injected_.empty()){selected_[i]=injected_;injected_.clear();SetWindowTextW(paths_[i],selected_[i].c_str());return 0;}
        if(folders_){if(pick_folder(h,chosen)){selected_[i]=chosen;SetWindowTextW(paths_[i],chosen.c_str());}return 0;}
        return 0;
    }
    if(msg==WM_COMMAND&&LOWORD(wp)==103&&!active_){
        try{if(!readonly_inputs()){SetWindowTextW(status_,L"Choose your originals outside the installer, build, toolchain and userdata folders.");return 0;}}
        catch(...){SetWindowTextW(status_,L"The selected files could not be read; choose them again.");return 0;}
        std::wstring missing=L"Choose your extracted ExeFS and RomFS folders.";
        for(int i=0;i<rows_;i++){const auto &p=selected_[i];DWORD a=p.empty()?INVALID_FILE_ATTRIBUTES:GetFileAttributesW(p.c_str());
            if(a!=INVALID_FILE_ATTRIBUTES&&(!folders_||(a&FILE_ATTRIBUTE_DIRECTORY)))continue;
            SetWindowTextW(status_,missing.c_str());
            try{directory(root_+L"\\installer");}catch(...){return 0;}
            HANDLE f=CreateFileW((root_+L"\\installer\\setup.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
            if(f!=INVALID_HANDLE_VALUE){std::string message=utf8(missing)+"\r\n";DWORD n;WriteFile(f,message.data(),(DWORD)message.size(),&n,nullptr);CloseHandle(f);}
            finished_=true;successful_=false;
            if(gui_test::enabled)gui_test::record(missing);return 0;}
        active_=true;finished_=successful_=false;error_.clear();percent_=0;enable_buttons(false);HANDLE worker=CreateThread(nullptr,0,build,nullptr,0,nullptr);
        if(worker)CloseHandle(worker);else{active_=false;enable_buttons(true);SetWindowTextW(status_,L"Setup could not start; close it and try again.");}return 0;
    }
    if(msg==STATUS){auto text=(std::wstring*)lp;SetWindowTextW(status_,text->c_str());delete text;
        percent_=(int)wp;
        bool graphics=wp>=94&&wp<98;
        auto style=GetWindowLongPtrW(progress_,GWL_STYLE);
        SetWindowLongPtrW(progress_,GWL_STYLE,graphics?style|PBS_MARQUEE:style&~PBS_MARQUEE);
        SendMessageW(progress_,PBM_SETMARQUEE,graphics,50);
        if(!graphics)SendMessageW(progress_,PBM_SETPOS,std::clamp((int)wp,0,100),0);
        if(gui_test::enabled)gui_test::snapshot(h,L"progress-"+std::to_wstring(wp));return 0;}
    if(msg==FINISHED){active_=false;enable_buttons(true);
        SendMessageW(progress_,PBM_SETMARQUEE,FALSE,0);
        SetWindowLongPtrW(progress_,GWL_STYLE,GetWindowLongPtrW(progress_,GWL_STYLE)&~PBS_MARQUEE);
        finished_=true;successful_=wp!=0;
        if(update_mode_&&wp&&!gui_test::enabled){
            // Updated: the launcher again, which now plays (it finds nothing newer).
            STARTUPINFOW si={sizeof si};PROCESS_INFORMATION p={};std::wstring launcher=root_+L"\\StrikersRecomp.exe",command=quote(launcher);
            if(CreateProcessW(launcher.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,root_.c_str(),&si,&p)){CloseHandle(p.hThread);CloseHandle(p.hProcess);DestroyWindow(h);return 0;}
        }
        const wchar_t *text=wp?(update_mode_?L"Updated! Open Strikers Recomp to play.":L"Ready! Open Strikers Recomp in this folder to play."):error_.c_str();
        SetWindowTextW(status_,text);if(wp)SendMessageW(progress_,PBM_SETPOS,100,0);
        // The script captures the final state on its next timer turn, after
        // control enable/paint messages have run; no synchronous stale image.
        if(gui_test::enabled)gui_test::record(text);return 0;}
    if(msg==WM_CLOSE&&active_){SetWindowTextW(status_,L"Setup is still working; wait for it to finish before closing.");return 0;}
    if(msg==WM_DESTROY){DeleteObject(font_);PostQuitMessage(0);return 0;}
    return DefWindowProcW(h,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    wchar_t path[32768];DWORD n=GetModuleFileNameW(nullptr,path,32768);if(!n||n>=32768)return 1;
    root_=path;root_=root_.substr(0,root_.find_last_of(L"\\/"));
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc>=2&&!wcscmp(argv[argc-1],L"--folders")){folders_=true;rows_=2;argc--;}
    try{if(argc==3&&!wcscmp(argv[1],L"--self-test"))gui_test::load(argv[2]);
        else if(argc==2&&!wcscmp(argv[1],L"--update"))update_mode_=true;
        else if(argc!=1)return 64;}
    catch(...){LocalFree(argv);return 64;}LocalFree(argv);
    children_=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION job={};
    job.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!children_||!SetInformationJobObject(children_,JobObjectExtendedLimitInformation,&job,sizeof job))return 65;
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE",L"1");
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 66;    // for the folder picker
    INITCOMMONCONTROLSEX cc={sizeof cc,ICC_PROGRESS_CLASS};InitCommonControlsEx(&cc);
    WNDCLASSEXW cls={sizeof cls};cls.lpfnWndProc=messages;cls.hInstance=instance;cls.lpszClassName=L"StrikersPlayerSetup";
    // The project's own icon (resource 1, tools/release/app_icon.py); the game's icon is for the game's shortcut and window.
    cls.hIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE);
    cls.hIconSm=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0);
    cls.hCursor=LoadCursor(nullptr,IDC_ARROW);cls.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClassExW(&cls);
    RECT size={0,0,700,490};AdjustWindowRect(&size,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    HWND h=CreateWindowW(cls.lpszClassName,update_mode_?L"Strikers Recomp — Update":L"Strikers Recomp — Setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                         CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,nullptr,nullptr,instance,nullptr);
    if(!h)return 2;ShowWindow(h,show);if(gui_test::enabled)SetTimer(h,21,200,nullptr);MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    CloseHandle(children_);return (int)msg.wParam;
}
