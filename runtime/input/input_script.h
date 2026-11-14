/* GPL-3.0-or-later. Frame-based, game-independent scripted controller input. */
#ifndef STRIKERS_INPUT_SCRIPT_H
#define STRIKERS_INPUT_SCRIPT_H
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define INPUT_SCRIPT_MAX 1024
/* Frames in each block are relative to its preceding wait (boot for block 0).
 * A wait is armed only after all intervals in its block have ended. */
typedef struct { long long first, count; char keys[32], event[160]; int player; } InputStep;
typedef struct { int begin, end; long long origin; } InputScriptState;
static int input_script_parse(FILE *f, InputStep *steps) {
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        InputStep s = {0}; int used = 0;
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        if (n == INPUT_SCRIPT_MAX) return -1;
        if (!strncmp(p, "wait ", 5)) {
            if (sscanf(p, "wait %159s %n", s.event, &used) != 1 || !used || (p[used] && p[used] != '#')) return -1;
            if (strncmp(s.event, "open:", 5) && strcmp(s.event, "movie")) return -1;
            if (!strcmp(s.event, "open:")) return -1;
            steps[n++] = s; continue;
        }
        if (*p == 'p') { int off = 0;
            if (sscanf(p, "p%d %n", &s.player, &off) != 1 || !off || s.player < 1 || s.player > 8) return -1;
            s.player--; p += off;
        }
        if (sscanf(p, " %lld+%lld:%31s %n", &s.first, &s.count, s.keys, &used) != 3 ||
            s.first < 0 || s.count <= 0 || s.first > LLONG_MAX - s.count || !used || (p[used] && p[used] != '#'))
            return -1;
        steps[n++] = s;
    }
    return n;
}
/* event_seen must find an event at/after the block's end, not a stale boot event.
 * It may observe a guest service from another thread; the caller synchronizes it. */
static int input_script_tick(const InputStep *s, int n, InputScriptState *st, long long frame,
                             int (*event_seen)(const char *, long long)) {
    int advanced = 0;
    for (;;) {
        int end = st->begin; long long last = 0;
        while (end < n && !s[end].event[0]) {
            if (s[end].first + s[end].count > last) last = s[end].first + s[end].count;
            end++;
        }
        st->end = end;
        if (end == n || frame - st->origin < last || !event_seen(s[end].event, st->origin + last)) break;
        st->begin = end + 1; st->origin = frame; advanced++;
    }
    return advanced;
}
static int input_script_player_down(const InputStep *s, const InputScriptState *st, long long frame, int player, char key) {
    frame -= st->origin;
    for (int i = st->begin; i < st->end; i++)
        if (s[i].player == player && frame >= s[i].first && frame - s[i].first < s[i].count && strchr(s[i].keys, key)) return 1;
    return 0;
}
static int input_script_down(const InputStep *steps, int n, long long frame, char key) {
    for (int i = 0; i < n; ++i)
        if (frame >= steps[i].first && frame - steps[i].first < steps[i].count && strchr(steps[i].keys, key)) return 1;
    return 0;
}
#endif
