/* Audit 07: runs a program exactly as the release launcher did (DEBUG_ONLY_THIS_PROCESS and a WaitForDebugEvent loop),
 * and counts every debug event: a07_dbgrun <report.txt> <exe> <args...>. Stdout and stderr are inherited. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static struct { DWORD code; unsigned long long n; } g_x[64];
static int g_nx;
int main(int argc, char **argv) {
    if (argc < 3) return 64;
    char cmd[32768] = "";
    for (int i = 2; i < argc; i++) { strcat(cmd, "\""); strcat(cmd, argv[i]); strcat(cmd, "\" "); }
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION p;
    if (!CreateProcessA(argv[2], cmd, NULL, NULL, TRUE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &p)) return 66;
    DEBUG_EVENT e;
    unsigned long long ev[10] = {0}, spent = 0;
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    DWORD code = 67;
    int initial = 1;
    while (WaitForDebugEvent(&e, INFINITE)) {
        QueryPerformanceCounter(&a);
        DWORD disp = DBG_CONTINUE;
        if (e.dwDebugEventCode < 10) ev[e.dwDebugEventCode]++;
        if (e.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            DWORD c = e.u.Exception.ExceptionRecord.ExceptionCode;
            if (initial && c == EXCEPTION_BREAKPOINT) initial = 0;
            else disp = DBG_EXCEPTION_NOT_HANDLED;
            int i;
            for (i = 0; i < g_nx; i++) if (g_x[i].code == c) break;
            if (i == g_nx && g_nx < 64) g_x[g_nx++].code = c;
            if (i < 64) g_x[i].n++;
        } else if (e.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (e.u.CreateProcessInfo.hFile) CloseHandle(e.u.CreateProcessInfo.hFile);
            CloseHandle(e.u.CreateProcessInfo.hProcess); CloseHandle(e.u.CreateProcessInfo.hThread);
        } else if (e.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) CloseHandle(e.u.CreateThread.hThread);
        else if (e.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && e.u.LoadDll.hFile) CloseHandle(e.u.LoadDll.hFile);
        else if (e.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) { code = e.u.ExitProcess.dwExitCode; ContinueDebugEvent(e.dwProcessId, e.dwThreadId, disp); break; }
        ContinueDebugEvent(e.dwProcessId, e.dwThreadId, disp);
        QueryPerformanceCounter(&b); spent += (unsigned long long)(b.QuadPart - a.QuadPart);
    }
    FILE *r = fopen(argv[1], "w");
    if (r) {
        static const char *const names[10] = {"", "exception", "create thread", "create process", "exit thread", "exit process", "load dll", "unload dll", "output debug string", "rip"};
        for (int i = 1; i < 10; i++) fprintf(r, "%s: %llu\n", names[i], ev[i]);
        for (int i = 0; i < g_nx; i++) fprintf(r, "exception %08lx: %llu\n", g_x[i].code, g_x[i].n);
        fprintf(r, "time in the event loop: %.3f s\nexit code %lu\n", (double)spent / (double)f.QuadPart, code);
        fclose(r);
    }
    return (int)code;
}
