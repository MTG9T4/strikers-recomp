/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: smo-recomp/runtime/gfx/gfx_options.h (0414341471307605b8cb5d0c1a9928db5fe84e07).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* Graphics options: strikers.ini. See gfx_options.c. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    char network_mode[32], zerotier_network[32];
    char group_service[256], group_code[16];
    int upscale; /* 0 linear, 1 AMD FSR1, 2 nearest, 3 integer nearest */
    float sharpness; /* 0..1 (1 strongest) */
    int monitor, pos_x, pos_y, position_valid, frame_cap, volume, pad_layout, mouse_menu;
    int keys[24][3]; /* Pro button bit order 0..15; left/right stick directions 16..23 */
    unsigned keys_set; /* explicit action overrides; unspecified actions retain legacy SDL bindings */
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env STRIKERS_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless fullscreen (desktop size), 2 exclusive fullscreen when supported (borderless fallback), 3 borderless window ( no frame, width x height, centred) */
    int width, height;       /* client size of the windowed modes (windowed, borderless window) */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env STRIKERS_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env STRIKERS_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for (Vulkan: nvn_vk_draw.c rs_scale, 0.5 .. 2.0, 0 = match window at startup; env STRIKERS_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a scene the draw is left out until they are ready; nvn_vk_pipelines.c skip_on; env STRIKERS_VK_NOHOLD wins) */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int speakers;            /* -1 auto (5.1 when Windows' output has 6+ channels), 0 stereo, 1 5.1 (nnaudio.c; restart) */
    int start_screen;        /* 1 (default): the PC start screen (Play / Online / Settings / Quit) over the game's boot, pc_overlay.cpp */
    int pause_unfocused;     /* unfocused: 0 run (the game goes on while the window is not active), 1 pause (nnmisc.c oe_watch stops it) */
    int hdr;                 /* 1: HDR10 output when Windows offers it (hdr_present.inc); strikers.ini hdr = on|off */
    int hdr_paper, hdr_peak; /* nits: the picture's white (80..400, 200) and its brightest highlights (200..4000, 400) */
    int button_pictures;     /* 0 automatic (from the pad), 1 the game's own (Switch), 2 Xbox, 3 PlayStation (texrepl.c; restart) */
    int ultrawide_hud;       /* ultrawide's interface: 0 stretched (markers on their players), 1 centred in 16:9 (markers drift inward) */
    int ultrawide;           /* 0 off, 1 the window's shape at start, else the aspect x 1000 (2370 = 21:9, 3556 = 32:9) (nvn_vk_draw.c rs_wide; restart) */
    int antialias;           /* 0 off, 1 SMAA 1x on the game's final picture (smaa_present.inc; strikers.ini antialiasing = off|smaa) */
    int fast_forward;        /* holding Tab runs the game this many times faster: 0 off, 2, 3, 4 (default), 8 (display.c) */
} GfxOptions;
#ifdef __cplusplus
extern "C" {
#endif
extern const char *const pc_action_names[24];
void pc_default_keys(int keys[24][3]);
int pc_parse_key(const char *s);
int gfx_options_save(const GfxOptions *o);
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
#ifdef __cplusplus
}
#endif
#endif
