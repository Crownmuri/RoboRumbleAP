/* DLL entry: loads the system winmm.dll, fills the forwarding table, reads rrpatch.ini and
   installs the features. Runs before the game's own startup code, so hooks are in place
   before the exe reads adjust.ini or creates its window. */
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include "gen/winmm_table.h"

extern void *rr_winmm_ptrs[RR_WINMM_COUNT];

RRConfig g_cfg;
char g_game_dir[MAX_PATH];
char g_ini_path[MAX_PATH];
char g_exe_name[MAX_PATH];
HMODULE g_real_winmm;
MCIERROR (WINAPI *real_mciSendCommandA)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);

static FILE *g_logf;
static CRITICAL_SECTION g_log_cs;

void rr_log(const char *fmt, ...)
{
    if (!g_logf) return;
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_logf, "[%7lu] ", GetTickCount() % 10000000);
    vfprintf(g_logf, fmt, ap);
    fputc('\n', g_logf);
    fflush(g_logf);
    LeaveCriticalSection(&g_log_cs);
    va_end(ap);
}

FARPROC rr_real(const char *name)
{
    return g_real_winmm ? GetProcAddress(g_real_winmm, name) : NULL;
}

/* Not a game function: a stub for exports the real dll lacks, so a call fails cleanly. */
static DWORD WINAPI missing_export(void) { return MMSYSERR_NOTSUPPORTED; }

static int ini_int(const char *sec, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, g_ini_path);
}

static void load_config(void)
{
    g_cfg.log            = ini_int("general", "log", 1);
    g_cfg.window_enabled = ini_int("window", "enabled", 1);
    g_cfg.borderless     = ini_int("window", "borderless", 0);
    g_cfg.scale          = ini_int("window", "scale", 2);
    g_cfg.smooth         = ini_int("window", "smooth", 0);
    g_cfg.fps_limit      = ini_int("window", "fps_limit", 60);
    g_cfg.clip_cursor    = ini_int("window", "clip_cursor", 1);
    g_cfg.music_enabled  = ini_int("music", "enabled", 1);
    g_cfg.music_volume   = ini_int("music", "volume", 100);
    GetPrivateProfileStringA("music", "folder", "MUSIC", g_cfg.music_folder,
                             sizeof g_cfg.music_folder, g_ini_path);
    g_cfg.override_enabled = ini_int("files", "enabled", 1);
    GetPrivateProfileStringA("files", "override_folder", "override", g_cfg.override_folder,
                             sizeof g_cfg.override_folder, g_ini_path);
    if (g_cfg.scale < 1) g_cfg.scale = 1;
    if (g_cfg.music_volume < 0) g_cfg.music_volume = 0;
    if (g_cfg.music_volume > 100) g_cfg.music_volume = 100;
}

static void init_paths(void)
{
    GetModuleFileNameA(NULL, g_game_dir, MAX_PATH);
    char *slash = strrchr(g_game_dir, '\\');
    const char *base = slash ? slash + 1 : g_game_dir;
    lstrcpynA(g_exe_name, base, MAX_PATH);
    CharLowerA(g_exe_name);
    if (slash) slash[1] = 0;
    lstrcpyA(g_ini_path, g_game_dir);
    lstrcatA(g_ini_path, "rrpatch.ini");
}

static BOOL load_real_winmm(void)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH); /* SysWOW64 for a 32-bit process */
    if (!n || n > MAX_PATH - 12) return FALSE;
    lstrcatA(path, "\\winmm.dll");
    g_real_winmm = LoadLibraryA(path);
    if (!g_real_winmm) return FALSE;
    for (int i = 0; i < RR_WINMM_COUNT; i++) {
        FARPROC p = rr_winmm_exports[i].name
            ? GetProcAddress(g_real_winmm, rr_winmm_exports[i].name)
            : GetProcAddress(g_real_winmm, MAKEINTRESOURCEA(rr_winmm_exports[i].ordinal));
        rr_winmm_ptrs[i] = p ? (void *)p : (void *)missing_export;
    }
    real_mciSendCommandA = (void *)rr_winmm_ptrs[RRW_mciSendCommandA];
    return TRUE;
}

/* The game's only EnumWindows call counts visible titled windows and warns "There are other
   tasks running in the background" when there is more than one, which on any modern
   desktop is always. Enumerating nothing keeps the count at zero. */
static BOOL WINAPI no_EnumWindows(WNDENUMPROC proc, LPARAM lp)
{
    (void)proc; (void)lp;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_log_cs);
    init_paths();
    if (!load_real_winmm()) return FALSE;
    load_config();
    if (g_cfg.log) {
        char logpath[MAX_PATH];
        lstrcpyA(logpath, g_game_dir);
        lstrcatA(logpath, "rrpatch.log");
        g_logf = fopen(logpath, "w");
    }
    rr_log("RoboRumble patch loaded into %s", g_exe_name);
    /* Only patch the game itself, not the launcher or the setup tools. */
    if (strncmp(g_exe_name, "rr_", 3) != 0) {
        rr_log("not a game exe, forwarding only");
        return TRUE;
    }
    /* The game paces itself with Sleep; at Windows' default 15.6 ms timer resolution its frames
       land on 15.6 or 31.2 ms (about 45 fps, uneven). 1 ms resolution gives even frames. */
    if (ini_int("general", "timer_1ms", 1)) {
        MMRESULT (WINAPI *tbp)(UINT) = (void *)rr_real("timeBeginPeriod");
        if (tbp && tbp(1) == TIMERR_NOERROR) rr_log("timer resolution 1 ms");
    }
    if (ini_int("general", "skip_task_warning", 1) &&
        rr_hook_iat(GetModuleHandleA(NULL), "USER32.dll", "EnumWindows", (void *)no_EnumWindows))
        rr_log("background-task warning disabled");
    vfs_install();
    cd_install();
    ddwin_install();
    glidewin_install();
    devui_install();
    soundlog_install();
    glidetrace_install();
    input_install();
    ap_install();
    return TRUE;
}
