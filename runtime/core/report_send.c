/* SPDX-License-Identifier: GPL-3.0-or-later
 * Opt-in problem reports: see report_send.h. The upload format is the report service's (cloudflare/reports/README.md):
 * POST <url>/v1/report, Content-Type application/zip, the zip as the body, the summary in X-Strikers-* headers
 * (percent-encoded UTF-8). The answer is {"id":"R-XXXXXX"} or {"error":"..."}.
 * HTTPS only (TLS 1.2 or newer, Windows' certificate checks, never disabled), except plain http to 127.0.0.1 for a local
 * test receiver; no redirects; the system proxy. */
#include <windows.h>
#include <winhttp.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "report_send.h"
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef STRIKERS_REPORT_URL_DEFAULT
#define STRIKERS_REPORT_URL_DEFAULT "https://reports.mtg9t4.fun"
#endif
#define REPORT_MAX_ZIP (10u * 1024 * 1024)

static void copy_trim(char *out, size_t cap, const char *from, size_t n) {
    while (n && (*from == ' ' || *from == '\t')) { from++; n--; }
    while (n && (from[n - 1] == ' ' || from[n - 1] == '\t' || from[n - 1] == '\r' || from[n - 1] == '\n')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, from, n); out[n] = 0;
}

void report_url(const char *ini_path, char *out, unsigned cap) {
    char v[512] = "";
    const char *env = getenv("STRIKERS_REPORT_URL");
    if (env) snprintf(v, sizeof v, "%s", env);
    else if (ini_path && *ini_path) GetPrivateProfileStringA("Network", "report_url", "", v, sizeof v, ini_path);
    char t[512]; copy_trim(t, sizeof t, v, strlen(v));
    if (!_stricmp(t, "off")) { out[0] = 0; return; }
    snprintf(out, cap, "%s", t[0] ? t : STRIKERS_REPORT_URL_DEFAULT);
}

void report_info_from_log(const char *log_path, ReportInfo *info) {
    FILE *f = fopen(log_path, "rb");
    if (!f) return;
    static char buf[1 << 18];
    size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[got] = 0;
    for (char *line = buf; line && *line; ) {
        char *end = strchr(line, '\n'); size_t n = end ? (size_t)(end - line) : strlen(line);
        if (!info->version[0] && n > 8 && !strncmp(line, "system: ", 8)) {
            /* Strikers Recomp <ver> | game code for <lvl> | Windows .. build .. | <cpu>, N logical processors | N GB memory | folder .. */
            const char *parts[8]; size_t lens[8]; int count = 0; const char *p = line + 8, *stop = line + n;
            while (p < stop && count < 8) {
                const char *bar = strstr(p, " | "); if (!bar || bar > stop) bar = stop;
                parts[count] = p; lens[count] = (size_t)(bar - p); count++;
                p = bar + (bar < stop ? 3 : 0); if (bar == stop) break;
            }
            /* A release names its version; a dev build says "(no release list)", sent as "dev". */
            static const char name[] = "Strikers Recomp ";
            if (count >= 1 && lens[0] > sizeof name - 1 && !strncmp(parts[0], name, sizeof name - 1)) {
                const char *v = parts[0] + sizeof name - 1;
                if (*v == '(') snprintf(info->version, sizeof info->version, "dev");
                else copy_trim(info->version, sizeof info->version, v, lens[0] - (sizeof name - 1));
            }
            if (count >= 3) copy_trim(info->os, sizeof info->os, parts[2], lens[2]);
            if (count >= 4) { const char *comma = memchr(parts[3], ',', lens[3]); copy_trim(info->cpu, sizeof info->cpu, parts[3], comma ? (size_t)(comma - parts[3]) : lens[3]); }
            if (count >= 5) { const char *mem = strstr(parts[4], " memory"); copy_trim(info->memory, sizeof info->memory, parts[4], mem && mem < parts[4] + lens[4] ? (size_t)(mem - parts[4]) : lens[4]); }
        } else if (!info->gpu[0] && n > 22 && !strncmp(line, "nvn_vk: using device ", 21)) {
            const char *colon = memchr(line, ':', n); colon = colon ? memchr(colon + 1, ':', (size_t)(line + n - colon - 1)) : NULL;
            const char *paren = colon ? strstr(colon, " (loader") : NULL;
            if (colon && paren && paren < line + n) copy_trim(info->gpu, sizeof info->gpu, colon + 1, (size_t)(paren - colon - 1));
        }
        if (info->version[0] && info->gpu[0]) break;
        line = end ? end + 1 : NULL;
    }
}

/* ---- redaction */
static size_t replace_ci(char *s, size_t n, size_t cap, const char *what, const char *with) {
    size_t wl = strlen(what), rl = strlen(with);
    if (!wl) return n;
    for (size_t i = 0; i + wl <= n; ) {
        if (!_strnicmp(s + i, what, wl)) {
            if (n - wl + rl >= cap) return n;
            memmove(s + i + rl, s + i + wl, n - i - wl); memcpy(s + i, with, rl); n = n - wl + rl; i += rl;
        } else i++;
    }
    return n;
}
/* \Users\<name> and /Users/<name> (any case, either slash) with the name of this account and of the profile folder;
 * the PC's own name where it stands alone (not inside a longer word). */
static size_t redact(char *s, size_t n, size_t cap) {
    char names[2][MAX_PATH] = {"", ""};
    DWORD len = MAX_PATH; if (!GetUserNameA(names[0], &len)) names[0][0] = 0;
    const char *profile = getenv("USERPROFILE");
    if (profile) { const char *slash = strrchr(profile, '\\'); snprintf(names[1], MAX_PATH, "%s", slash ? slash + 1 : profile); }
    for (int k = 0; k < 2; k++) {
        if (strlen(names[k]) < 2 || (k == 1 && !_stricmp(names[0], names[1]))) continue;
        static const char *const seps[][2] = {{"\\Users\\", "\\"}, {"/Users/", "/"}, {"\\\\Users\\\\", "\\\\"}};
        for (int i = 0; i < 3; i++) {
            char what[MAX_PATH + 16], with[32];
            /* only a whole folder name: followed by a separator, quote, space or the end */
            snprintf(what, sizeof what, "%s%s%s", seps[i][0], names[k], seps[i][1]);
            snprintf(with, sizeof with, "%s<user>%s", seps[i][0], seps[i][1]);
            n = replace_ci(s, n, cap, what, with);
            for (const char *tail = " \"'\r\n"; *tail; tail++) {
                snprintf(what, sizeof what, "%s%s%c", seps[i][0], names[k], *tail);
                snprintf(with, sizeof with, "%s<user>%c", seps[i][0], *tail);
                n = replace_ci(s, n, cap, what, with);
            }
        }
    }
    char pc[MAX_COMPUTERNAME_LENGTH + 2]; DWORD pl = sizeof pc;
    if (GetComputerNameA(pc, &pl) && pl >= 4) {
        for (size_t i = 0; i + pl <= n; ) {
            int word_before = i && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'), word_after = i + pl < n && (isalnum((unsigned char)s[i + pl]) || s[i + pl] == '_');
            if (!_strnicmp(s + i, pc, pl) && !word_before && !word_after) {
                if (n - pl + 4 >= cap) break;
                memmove(s + i + 4, s + i + pl, n - i - pl); memcpy(s + i, "<pc>", 4); n = n - pl + 4; i += 4;
            } else i++;
        }
    }
    return n;
}
int report_redact_file(const char *path, unsigned max_bytes) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size; if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return 0; }
    unsigned long long total = (unsigned long long)size.QuadPart;
    static const char marker[] = "\r\n... (the middle of this file was left out of the report to keep it small) ...\r\n";
    unsigned long long head = total, tail = 0;
    if (max_bytes && total > max_bytes) { head = max_bytes / 4; tail = max_bytes - head; }
    size_t cap = (size_t)(head + tail) + sizeof marker + 65536;
    char *s = malloc(cap); if (!s) { CloseHandle(h); return 0; }
    DWORD got; size_t n = 0; int ok = 1;
    if (!ReadFile(h, s, (DWORD)head, &got, NULL)) ok = 0; n = got;
    if (ok && tail) {
        memcpy(s + n, marker, sizeof marker - 1); n += sizeof marker - 1;
        LARGE_INTEGER at; at.QuadPart = (LONGLONG)(total - tail);
        if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN) || !ReadFile(h, s + n, (DWORD)tail, &got, NULL)) ok = 0; else n += got;
    }
    CloseHandle(h);
    if (ok) n = redact(s, n, cap);
    if (ok) {
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD put = 0;
        ok = h != INVALID_HANDLE_VALUE && WriteFile(h, s, (DWORD)n, &put, NULL) && put == n;
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    free(s);
    return ok;
}
int report_copy_settings(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return 0;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
    static const char *const private_keys[] = {"group_code", "room_key", "group_service"};
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        const char *eq = strchr(p, '='); int hidden = 0;
        for (int i = 0; eq && i < 3; i++) {
            size_t k = strlen(private_keys[i]);
            if (!_strnicmp(p, private_keys[i], k) && (p[k] == ' ' || p[k] == '\t' || p[k] == '=')) {
                const char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
                hidden = 1;
                if (*v == '\r' || *v == '\n' || !*v) fputs(line, out);   /* empty: nothing private */
                else fprintf(out, "%.*s= (removed from the report)\r\n", (int)(eq - line), line);
            }
        }
        if (!hidden) fputs(line, out);
    }
    int ok = !ferror(in) && !ferror(out);
    fclose(in); if (fclose(out)) ok = 0;
    return ok;
}

/* ---- upload */
static void percent(const char *in, wchar_t *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF"; size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~ ()", *p)) out[o++] = *p == ' ' ? L' ' : (wchar_t)*p;
        else { out[o++] = L'%'; out[o++] = (wchar_t)hex[*p >> 4]; out[o++] = (wchar_t)hex[*p & 15]; }
    }
    out[o] = 0;
}
static void json_string(const char *body, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char k[32]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(body, k); if (!p) return;
    p = strchr(p + strlen(k), ':'); if (!p) return;
    while (*++p == ' ') {}
    if (*p != '"') return;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < cap; p++) { if (*p == '\\' && p[1]) p++; out[o++] = *p; }
    out[o] = 0;
}
int report_send(const char *url, const char *zip_path, const ReportInfo *info, char *id, unsigned id_cap,
                char *message, unsigned message_cap) {
    id[0] = 0; message[0] = 0;
    if (!url || !*url) { snprintf(message, message_cap, "Sending reports is turned off in strikers.ini."); return 0; }
    wchar_t wurl[600], host[256], path[512];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wurl, 600)) { snprintf(message, message_cap, "The report address in strikers.ini is not valid."); return 0; }
    URL_COMPONENTS c; memset(&c, 0, sizeof c); c.dwStructSize = sizeof c;
    c.lpszHostName = host; c.dwHostNameLength = 256; c.lpszUrlPath = path; c.dwUrlPathLength = 512;
    c.dwUserNameLength = c.dwPasswordLength = c.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &c) || !c.dwHostNameLength || c.dwUserNameLength || c.dwPasswordLength || c.dwExtraInfoLength) { snprintf(message, message_cap, "The report address in strikers.ini is not valid."); return 0; }
    int secure = c.nScheme == INTERNET_SCHEME_HTTPS;
    /* plain http only to this PC itself (a local test receiver): nothing unencrypted leaves the PC */
    if (!secure && !(c.nScheme == INTERNET_SCHEME_HTTP && !wcscmp(host, L"127.0.0.1"))) { snprintf(message, message_cap, "The report address must use https."); return 0; }
    wchar_t target[600]; size_t pl = wcslen(path); while (pl && path[pl - 1] == L'/') path[--pl] = 0;
    swprintf(target, 600, L"%ls/v1/report", path);

    HANDLE file = CreateFileA(zip_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) { snprintf(message, message_cap, "The report file could not be read."); return 0; }
    LARGE_INTEGER size; GetFileSizeEx(file, &size);
    if (size.QuadPart < 22 || size.QuadPart > REPORT_MAX_ZIP) { CloseHandle(file); snprintf(message, message_cap, "The report is too big to send (%.1f MB; the limit is 10 MB).", size.QuadPart / 1048576.0); return 0; }

    wchar_t headers[4096], v[3200]; size_t at = 0;
    const struct { const wchar_t *name; const char *value; } fields[] = {
        {L"X-Strikers-Version", info->version[0] ? info->version : "unknown"}, {L"X-Strikers-Kind", info->kind[0] ? info->kind : "menu"},
        {L"X-Strikers-GPU", info->gpu}, {L"X-Strikers-CPU", info->cpu}, {L"X-Strikers-OS", info->os},
        {L"X-Strikers-Memory", info->memory}, {L"X-Strikers-Description", info->description}};
    at += (size_t)swprintf(headers, 4096, L"Content-Type: application/zip\r\n");
    for (int i = 0; i < 7; i++) if (fields[i].value[0]) {
        percent(fields[i].value, v, 3200);
        int w = swprintf(headers + at, 4096 - at, L"%ls: %ls\r\n", fields[i].name, v); if (w > 0) at += (size_t)w;
    }

    int sent = 0; DWORD status = 0;
    HINTERNET session = WinHttpOpen(L"StrikersReport/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), connection = NULL, r = NULL;
    if (!session) session = WinHttpOpen(L"StrikersReport/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) {
        WinHttpSetTimeouts(session, 10000, 15000, 60000, 30000);
        DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) { tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls); }
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
        connection = WinHttpConnect(session, host, c.nPort, 0);
    }
    if (connection) r = WinHttpOpenRequest(connection, L"POST", target, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (r && WinHttpSendRequest(r, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)size.QuadPart, 0)) {
        static char chunk[1 << 16]; DWORD got, put; sent = 1;
        while (sent && ReadFile(file, chunk, sizeof chunk, &got, NULL) && got) if (!WinHttpWriteData(r, chunk, got, &put) || put != got) sent = 0;
        if (sent && WinHttpReceiveResponse(r, NULL)) {
            DWORD len = sizeof status; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            char body[2048]; DWORD total = 0, avail;
            while (WinHttpQueryDataAvailable(r, &avail) && avail && total + 1 < sizeof body) {
                DWORD want = avail < sizeof body - 1 - total ? avail : (DWORD)(sizeof body - 1 - total);
                if (!WinHttpReadData(r, body + total, want, &got) || !got) break;
                total += got;
            }
            body[total] = 0;
            if (status == 200) { json_string(body, "id", id, id_cap); if (!id[0]) { sent = 0; snprintf(message, message_cap, "The report service gave an answer the game does not understand."); } }
            else {
                char error[400]; json_string(body, "error", error, sizeof error); sent = 0;
                if (error[0]) snprintf(message, message_cap, "%s", error);
                else snprintf(message, message_cap, "The report service answered %lu.", (unsigned long)status);
            }
        } else if (sent) sent = 0;
    }
    if (!sent && !message[0]) snprintf(message, message_cap, "Couldn't reach the report service (no internet?).");
    if (r) WinHttpCloseHandle(r);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    CloseHandle(file);
    return sent;
}
