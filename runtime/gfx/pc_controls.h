/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CONTROLS_H
#define PC_CONTROLS_H
#include "gfx_options.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { PC_CLOSED, PC_SETTINGS, PC_START, PC_MENU };
void pc_menu_set(int mode);
int pc_menu_mode(void);
void pc_open_settings(int online_tab);
int pc_settings_tab_take(void);
void pc_start_screen_maybe(int visible);
void pc_settings_toggle(void);
int pc_settings_open(void);
int pc_settings_epoch(void);
int pc_input_gate(unsigned player, unsigned long long buttons, int lx, int ly, int rx, int ry);
void pc_pad_navigation(unsigned long long *buttons, int *lx, int *ly);
int pc_key_down(int action, int legacy_vk);
int pc_key_override(int action);
void pc_settings_get(GfxOptions *out);
void pc_settings_apply(const GfxOptions *o);
void pc_scene_hint(int scene);
int pc_cursor_visible(void);
void pc_window_geometry(int x, int y, int width, int height);
int pc_ui_needed(void);
void pc_ui_draw(void *instance, void *gpu, void *device, unsigned family, void *queue,
                void *cmd, unsigned long long image, int format, unsigned width, unsigned height,
                void *(*loader)(void *, const char *));
void pc_ui_shutdown(void);
void pc_ui_reset_targets(void);
void pc_capture_request(void);
#ifdef STRIKERS_PC_SCALE_TEST
void pc_scale_test_request(int percent);
#endif
int pc_ui_message(void *window, unsigned message, unsigned long long w, long long l);
#ifdef __cplusplus
}
#endif
#endif
