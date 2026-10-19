#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

/* ---- config (rrpatch.ini next to the game exe) ---- */
typedef struct {
    int  log;
    /* [window] */
    int  window_enabled;   /* fake DirectDraw for the software renderer */
    int  borderless;       /* 1 = borderless window covering the monitor */
    int  scale;            /* initial client size = 640x480 * scale */
    int  smooth;           /* 1 = HALFTONE stretching, 0 = nearest */
    int  fps_limit;        /* caps Flip(); 0 = off */
    int  clip_cursor;      /* confine the cursor to the window while it is active */
    /* [music] */
    int  music_enabled;
    char music_folder[MAX_PATH];
    int  music_volume;     /* 0-100, multiplied with the in-game music volume */
    /* [files] */
    int  override_enabled;
    char override_folder[64];
} RRConfig;

extern RRConfig g_cfg;
extern char g_game_dir[MAX_PATH];   /* folder of the exe, with trailing backslash */
extern char g_ini_path[MAX_PATH];   /* rrpatch.ini */
extern char g_exe_name[MAX_PATH];   /* lower-case file name of the exe, e.g. rr_soft.exe */
extern HMODULE g_real_winmm;

void rr_log(const char *fmt, ...);

/* resolved functions of the real winmm.dll (never call our own exports for these) */
extern MCIERROR (WINAPI *real_mciSendCommandA)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
FARPROC rr_real(const char *name);

/* ---- hooks.c ---- */
/* Replaces an import of the exe; returns the previous pointer or NULL. */
void *rr_hook_iat(HMODULE module, const char *dll, const char *func, void *replacement);
/* Finds `len` bytes in the exe's .text section; returns the address or NULL if not exactly one match. */
BYTE *rr_find_unique(const BYTE *pattern, size_t len);
/* Writes a 5-byte jmp at `at` to `to` (must have at least `len` bytes of whole instructions). */
BOOL rr_write_jmp(BYTE *at, void *to, size_t len);

/* ---- feature entry points ---- */
void cd_install(void);
void ddwin_install(void);
void glidewin_install(void);
int rr_have_dgvoodoo(void);
int rr_nglide_managed(void);   /* nGlide with the patch owning the window */
HWND rr_game_hwnd(void);
void devui_install(void);
void soundlog_install(void);
void glidetrace_install(void);
void input_install(void);
void ap_install(void);
void apdlg_generate(void);      /* apdlg.c: the Archipelago menus in the briefing dialogs (needs vfs.c) */
void apdlg_tick(void);          /* apdlg.c: dialog variables, on the game thread */

/* ---- vfs.c ---- */
char *rr_pak_read(const char *pak, const char *entry, DWORD *size);   /* pak relative to the game dir */
int rr_vfs_generate(const char *name, const void *data, DWORD size);   /* the game gets this file instead */

/* ---- shared game locations (devui.c finds them by pattern, the first call does the work) ---- */
typedef struct {
    BYTE *state;        /* +0 side (0 A, 1 B), +4 mission index 0-15, +8 shop money,
                           +0x0C available[22], +0x22 owned[22]: 9 boosters, 10 base upgrades, 3 inventions */
    char *heads, *legs, *boosters;   /* '+'/'-' masks the robot builder reads */
    DWORD *winflag;     /* 1 = mission won, 2 = lost */
    BYTE *restart;
} RRGame;
const RRGame *rr_game(void);    /* NULL if the game data wasn't found */

/* ---- game mode (input.c): GUI_MODE_* of the main loop ---- */
#define RR_MODE_LOADING 1
#define RR_MODE_MENU 3
#define RR_MODE_GAMEPLAY 4
#define RR_MODE_SHOP 5
#define RR_MODE_BRIEFING 7
#define RR_MODE_LOADLEVEL 8
#define RR_MODE_END_OF_LEVEL 10
extern volatile DWORD g_gui_mode;
int rr_watch_modes(void);       /* installs the mode hook once; 0 if not found */
void rr_on_mode(void (*fn)(DWORD mode));   /* called on the game thread when the next screen is chosen */
void vfs_install(void);
