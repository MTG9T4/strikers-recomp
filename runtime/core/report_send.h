/* SPDX-License-Identifier: GPL-3.0-or-later
 * Opt-in problem reports: a report zip (bug_report.c, or the launcher after a crash) sent to the developer's report
 * service (cloudflare/reports). Nothing calls this unless the player pressed Send or answered Yes. Plain C, built into
 * the game and into the launcher (tools/release/crash_launcher.cpp). */
#ifndef STRIKERS_REPORT_SEND_H
#define STRIKERS_REPORT_SEND_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char version[33], kind[8], gpu[129], cpu[129], os[65], memory[17], description[1001];
} ReportInfo;
/* The service's address: [Network] report_url in this strikers.ini, else the built-in one. "" when reports are off
 * (report_url = off) or no service is set. */
void report_url(const char *ini_path, char *out, unsigned cap);
/* Version, processor, Windows, memory (the log's "system:" line) and graphics card ("nvn_vk: using device") from a
 * game log; fields not found stay empty. */
void report_info_from_log(const char *log_path, ReportInfo *info);
/* A text file of the report, rewritten in place before zipping: C:\Users\<account> (either slash, any case) becomes
 * C:\Users\<user>, this PC's name becomes <pc>, and a file over max_bytes keeps its first quarter and last three
 * quarters around a marker. 1 on success. */
int report_redact_file(const char *path, unsigned max_bytes);
/* strikers.ini for a report: private values (group_code, room_key, group_service) are replaced. 1 on success. */
int report_copy_settings(const char *from, const char *to);
/* Uploads the zip. 1: sent, id = "R-XXXXXX". 0: not sent, message says why in plain words. Blocks up to ~60 s: call it
 * on a worker thread. */
int report_send(const char *url, const char *zip_path, const ReportInfo *info, char *id, unsigned id_cap,
                char *message, unsigned message_cap);
#ifdef __cplusplus
}
#endif
#endif
