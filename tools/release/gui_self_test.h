// SPDX-License-Identifier: GPL-3.0-or-later
// Only an application's own UI thread uses this helper; no desktop automation.
#pragma once
#include <objidl.h>
#include <gdiplus.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
namespace gui_test {
static std::vector<std::wstring> lines;
static size_t cursor;
static std::filesystem::path folder;
static ULONGLONG deadline;
static ULONG_PTR token;
static bool enabled;
static void load(const wchar_t *script) {
    std::ifstream input(script, std::ios::binary);
    if(!input) throw std::runtime_error("Cannot open self-test script");
    std::string line;
    while(std::getline(input,line)) {
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.empty() || line[0]=='#')continue;
        int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,line.data(),(int)line.size(),nullptr,0);
        if(!n)throw std::runtime_error("Self-test script must be UTF-8");
        std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,line.data(),(int)line.size(),w.data(),n);
        lines.push_back(w);
    }
    wchar_t module[32768];DWORD count=GetModuleFileNameW(nullptr,module,32768);
    if(!count||count>=32768)throw std::runtime_error("Cannot locate self-test output folder");
    // A script may be read from elsewhere; every artifact stays app-local.
    folder=std::filesystem::path(module).parent_path()/L"screenshots";
    std::filesystem::create_directory(folder);
    Gdiplus::GdiplusStartupInput init;
    if(Gdiplus::GdiplusStartup(&token,&init,nullptr)!=Gdiplus::Ok)throw std::runtime_error("PNG encoder unavailable");
    enabled=true;
}
static void snapshot(HWND window,const std::wstring &name) {
    if(name.empty() || name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::wstring::npos)
        throw std::runtime_error("Invalid screenshot name");
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    RECT r;GetWindowRect(window,&r);HDC dc=GetDC(window), mem=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right-r.left,r.bottom-r.top);
    HGDIOBJ old=SelectObject(mem,bitmap);
    BOOL printed=PrintWindow(window,mem,2);
    SelectObject(mem,old);
    CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    Gdiplus::Status saved=Gdiplus::GenericError;
    {Gdiplus::Bitmap image(bitmap,nullptr);if(printed)saved=image.Save((folder/(name+L".png")).c_str(),&png,nullptr);}
    DeleteObject(bitmap);DeleteDC(mem);ReleaseDC(window,dc);
    if(!printed || saved!=Gdiplus::Ok)throw std::runtime_error("PrintWindow PNG capture failed");
}
static void record(const std::wstring &text) {
    std::wofstream out(folder.parent_path()/L"self-test.log",std::ios::app);out<<text<<L"\n";
}
}
