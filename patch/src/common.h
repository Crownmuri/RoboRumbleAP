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
void vfs_install(void);
