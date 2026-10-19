/* Windowed mode for the Glide renderer (rr_glide.exe on nGlide).

   nGlide (bundled with the GOG release) can already run windowed; it switches when it sees
   Alt+Enter. After every grSstWinOpen (the game opens the Glide display for the menus and
   again for missions) this checks whether the window still covers the whole monitor and, if
   so, sends nGlide that Alt+Enter. */
#include "common.h"
#include <stdio.h>

typedef int (__stdcall *grSstWinOpen_t)(DWORD hwnd, int res, int refresh, int color_fmt, int origin, int ncolor, int naux);
static grSstWinOpen_t real_grSstWinOpen;
static HWND g_glide_hwnd;
static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND hwnd = real_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (hwnd && !g_glide_hwnd && cls && HIWORD((DWORD_PTR)cls) && lstrcmpiA(cls, "RoboRumble") == 0)
        g_glide_hwnd = hwnd;
    return hwnd;
}

/* nGlide's own fullscreen window: covers the monitor, or (with the mode switch blocked) is
   a frameless window; its windowed mode has a frame (WS_DLGFRAME). */
static int nglide_fullscreen(HWND hwnd);

static int covers_monitor(HWND hwnd)
{
    RECT r;
    MONITORINFO mi = { sizeof mi };
    if (!GetWindowRect(hwnd, &r) || !GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return 0;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

static int nglide_fullscreen(HWND hwnd)
{
    return covers_monitor(hwnd) || !(GetWindowLongA(hwnd, GWL_STYLE) & WS_DLGFRAME);
}

static void log_window(const char *when, HWND hwnd)
{
    RECT r = {0}, c = {0};
    GetWindowRect(hwnd, &r);
    GetClientRect(hwnd, &c);
    rr_log("glide: %s window %ld,%ld-%ld,%ld client %ldx%ld style %08lx", when, r.left, r.top, r.right, r.bottom,
           c.right, c.bottom, (DWORD)GetWindowLongA(hwnd, GWL_STYLE));
}

static void press_alt_enter(void)
{
    INPUT in[4] = {0};
    for (int i = 0; i < 4; i++) in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MENU;
    in[1].ki.wVk = VK_RETURN;
    in[2].ki.wVk = VK_RETURN; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_MENU;   in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

/* nGlide reads the real keyboard, so the switch is a real Alt+Enter, pressed once the window
   has settled and only while it is in front (the keys would otherwise go to another app). */
static DWORD WINAPI switch_thread(LPVOID arg)
{
    HWND w = (HWND)arg;
    Sleep(700);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!IsWindow(w) || !nglide_fullscreen(w)) break;
        if (GetForegroundWindow() != w) { rr_log("glide: not in front, leaving fullscreen"); break; }
        LONG style = GetWindowLongA(w, GWL_STYLE);
        press_alt_enter();
        /* wait for nGlide to react before considering another press, or it toggles back */
        for (int t = 0; t < 30 && IsWindow(w) && GetWindowLongA(w, GWL_STYLE) == style; t++) Sleep(100);
        Sleep(500);
        log_window("after alt+enter", w);
        if (GetWindowLongA(w, GWL_STYLE) != style) break;
    }
    return 0;
}

static int __stdcall hook_grSstWinOpen(DWORD hwnd, int res, int refresh, int color_fmt, int origin, int ncolor, int naux)
{
    int ok = real_grSstWinOpen(hwnd, res, refresh, color_fmt, origin, ncolor, naux);
    HWND w = hwnd ? (HWND)hwnd : g_glide_hwnd;
    rr_log("glide: grSstWinOpen(res %d) -> %d", res, ok);
    if (!ok || !w) return ok;
    log_window("after open", w);
    if (nglide_fullscreen(w)) CloseHandle(CreateThread(NULL, 0, switch_thread, w, 0, NULL));
    return ok;
}

/* Robot building menu trails under dgVoodoo.
   Every frame the shop only clears the depth buffer and copies its background (shop.tga)
   into the frame buffer pixel by pixel, writing 0x0000 where the picture has its key colour:
   the hole the spinning robot preview sits in. Real Voodoo cards and nGlide write that black;
   dgVoodoo takes 0x0000 in such writes as "untouched", so old frames of the robot stay as
   trails. The copy now writes 0x0001 instead (black with the lowest blue bit, invisible).
   Patched: xor edx,edx / mov ecx,esi / and ecx,0FFFFh before the copy call. */
static const BYTE k_shop_black_sig[] = {
    0x33, 0xD2, 0x8B, 0xCE, 0x81, 0xE1, 0xFF, 0xFF, 0x00, 0x00,
    0x89, 0x44, 0x24, 0x14, 0x89, 0x4C, 0x24, 0x18, 0x89, 0x6C, 0x24, 0x0C, 0x89, 0x54, 0x24, 0x1C, 0xE8,
};
void *g_shop_black_resume;
DWORD g_shop_black_value;
/* Writing a different black didn't help on its own: the game rewrites the same value every
   frame and dgVoodoo apparently only passes on LFB pixels that changed since the previous
   write, so the hole is never refreshed and the 3D robot drawn into it piles up. The fill
   now alternates between 0x0000 and 0x0020 (darkest green, ~1/250 brightness). The
   background is copied in three pieces per frame, an odd number, so every piece alternates
   from frame to frame. */
__asm__(
    ".text\n"
    ".globl _shop_black_stub\n"
    "_shop_black_stub:\n"
    "    movl _g_shop_black_value, %edx\n"
    "    xorl $0x20, %edx\n"
    "    movl %edx, _g_shop_black_value\n"
    "    movl %esi, %ecx\n"
    "    andl $0xffff, %ecx\n"
    "    jmp *_g_shop_black_resume\n");
void shop_black_stub(void);

static void shop_black_fix(void)
{
    if (strcmp(g_exe_name, "rr_soft.exe") == 0) return;   /* draws into its own memory */
    BYTE *at = rr_find_unique(k_shop_black_sig, sizeof k_shop_black_sig);
    if (!at) { rr_log("glide: shop background copy not found"); return; }
    g_shop_black_resume = at + 10;
    if (rr_write_jmp(at, (void *)shop_black_stub, 10)) rr_log("glide: shop preview black fix at %p", at);
}

/* nGlide switches the display mode with ChangeDisplaySettingsA for its fullscreen, which
   makes Windows rearrange windows on other monitors (bad when streaming). Inside nGlide that
   call is now a no-op: the desktop mode stays, nGlide's "fullscreen" is just a window, and
   the Alt+Enter below turns it into a normal one. */
static LONG WINAPI no_ChangeDisplaySettingsA(DEVMODEA *mode, DWORD flags)
{
    if (mode) rr_log("glide: blocked nGlide display mode change to %lux%lu", mode->dmPelsWidth, mode->dmPelsHeight);
    return DISP_CHANGE_SUCCESSFUL;
}

/* Direct3D 9 backend: a device created with Windowed = FALSE switches the display mode by
   itself, so every device nGlide creates or resets is made windowed. Present parameters:
   Windowed at +32, FullScreen_RefreshRateInHz at +48. */
typedef HRESULT (WINAPI *CreateDevice_t)(void *, UINT, DWORD, HWND, DWORD, DWORD *, void **);
typedef HRESULT (WINAPI *Reset_t)(void *, DWORD *);
static CreateDevice_t real_CreateDevice;
static Reset_t real_Reset;

static void force_windowed(DWORD *pp, const char *what)
{
    if (!pp) return;
    if (!pp[8]) rr_log("glide: nGlide %s %lux%lu fullscreen -> windowed", what, pp[0], pp[1]);
    pp[8] = TRUE;
    pp[12] = 0;
}

static void patch_slot(void **vt, int index, void *fn, void **orig)
{
    if (vt[index] == fn) return;
    DWORD old;
    VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old);
    *orig = vt[index];
    vt[index] = fn;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
}

static HRESULT WINAPI windowed_Reset(void *dev, DWORD *pp)
{
    force_windowed(pp, "Reset");
    return real_Reset(dev, pp);
}

static HRESULT WINAPI windowed_CreateDevice(void *d3d, UINT adapter, DWORD type, HWND focus, DWORD flags, DWORD *pp, void **out)
{
    force_windowed(pp, "CreateDevice");
    HRESULT hr = real_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) patch_slot(*(void ***)*out, 16, (void *)windowed_Reset, (void **)&real_Reset);
    return hr;
}

typedef void *(WINAPI *Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t real_Direct3DCreate9;
static void *WINAPI windowed_Direct3DCreate9(UINT sdk)
{
    void *d3d = real_Direct3DCreate9(sdk);
    if (d3d) patch_slot(*(void ***)d3d, 16, (void *)windowed_CreateDevice, (void **)&real_CreateDevice);
    return d3d;
}

static FARPROC (WINAPI *real_GetProcAddress)(HMODULE, LPCSTR);
static FARPROC WINAPI log_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC p = real_GetProcAddress(m, name);
    if (p && HIWORD((DWORD_PTR)name) && strcmp(name, "Direct3DCreate9") == 0) {
        real_Direct3DCreate9 = (Direct3DCreate9_t)p;
        rr_log("glide: nGlide uses Direct3D 9, devices forced windowed");
        return (FARPROC)windowed_Direct3DCreate9;
    }
    if (HIWORD((DWORD_PTR)name) && (strstr(name, "Direct3DCreate") || strncmp(name, "vkCreate", 8) == 0 ||
                                    strcmp(name, "vkGetInstanceProcAddr") == 0))
        rr_log("glide: nGlide uses %s", name);
    return p;
}

/* nGlide still believes it runs fullscreen and keeps resizing, restyling and confining the
   cursor to the game window. The patch owns that window now (see ddwin.c), so those calls
   are ignored for it. */
static BOOL (WINAPI *ng_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static LONG (WINAPI *ng_SetWindowLongA)(HWND, int, LONG);
static BOOL (WINAPI *ng_ShowWindow)(HWND, int);

static BOOL WINAPI ng_hook_SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT f)
{
    if (h && h == rr_game_hwnd()) return TRUE;
    return ng_SetWindowPos(h, after, x, y, w, hh, f);
}

/* nGlide puts its own window procedure in front of the patch's, and it doesn't pass the close on: Alt+F4 or the
   close button only took the window away and left the process running. A guard goes in front of it again. */
static WNDPROC g_ng_wndproc;
static LRESULT CALLBACK close_guard_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE)) {
        rr_log("window: closed by the user, exiting");
        ClipCursor(NULL);
        ExitProcess(0);
    }
    return CallWindowProcA(g_ng_wndproc, h, msg, wp, lp);
}

static LONG WINAPI ng_hook_SetWindowLongA(HWND h, int index, LONG v)
{
    if (h && h == rr_game_hwnd() && (index == GWL_STYLE || index == GWL_EXSTYLE)) return GetWindowLongA(h, index);
    LONG prev = ng_SetWindowLongA(h, index, v);
    if (h && h == rr_game_hwnd() && index == GWL_WNDPROC && (WNDPROC)v != close_guard_proc) {
        g_ng_wndproc = (WNDPROC)v;
        ng_SetWindowLongA(h, GWL_WNDPROC, (LONG)close_guard_proc);
        rr_log("glide: nGlide subclassed the game window, close guard in front of it");
    }
    return prev;
}

static BOOL WINAPI ng_hook_ShowWindow(HWND h, int cmd)
{
    if (h && h == rr_game_hwnd()) return TRUE;
    return ng_ShowWindow(h, cmd);
}

static BOOL WINAPI ng_hook_ClipCursor(const RECT *r) { (void)r; return TRUE; }

static void nglide_no_mode_switch(void)
{
    HMODULE ng = GetModuleHandleA("glide2x.dll");
    if (!ng) return;
    if (rr_hook_iat(ng, "USER32.dll", "ChangeDisplaySettingsA", (void *)no_ChangeDisplaySettingsA))
        rr_log("glide: nGlide display mode changes blocked");
    real_GetProcAddress = rr_hook_iat(ng, "KERNEL32.dll", "GetProcAddress", (void *)log_GetProcAddress);
    ng_SetWindowPos = rr_hook_iat(ng, "USER32.dll", "SetWindowPos", (void *)ng_hook_SetWindowPos);
    ng_SetWindowLongA = rr_hook_iat(ng, "USER32.dll", "SetWindowLongA", (void *)ng_hook_SetWindowLongA);
    ng_ShowWindow = rr_hook_iat(ng, "USER32.dll", "ShowWindow", (void *)ng_hook_ShowWindow);
    rr_hook_iat(ng, "USER32.dll", "ClipCursor", (void *)ng_hook_ClipCursor);
}

/* [general] log_fps=1: frames per second, slowest frame and time spent waiting in
   grBufferSwap, every 5 s. show_fps=1: the same in the window title, twice a second. */
void prof_frame(void);
void texcache_install(void);
void texcache_report(void);
typedef void (__stdcall *grBufferSwap_t)(int);
static grBufferSwap_t real_grBufferSwap;
static int g_log_fps, g_show_fps;
static int g_swap_override = -1;   /* [window] glide_swap_interval; -1 = game's own */
static void __stdcall fps_grBufferSwap(int interval)
{
    static LARGE_INTEGER freq, start, last, tstart;
    static int frames, tframes;
    static double worst, swap_ms, tworst, tswap;
    LARGE_INTEGER before, now;
    /* The game asks for a swap every second refresh (30 fps). Its logic is time based (it also
       runs on the software renderer, which flips every refresh), so 60 fps is just smoother. */
    if (g_swap_override >= 0) interval = g_swap_override;
    QueryPerformanceCounter(&before);
    real_grBufferSwap(interval);
    prof_frame();
    texcache_report();
    if (!g_log_fps && !g_show_fps) return;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); start = last = tstart = now; return; }
    double ms = 1000.0 / freq.QuadPart;
    double dt = (now.QuadPart - last.QuadPart) * ms, sw = (now.QuadPart - before.QuadPart) * ms;
    last = now;
    if (dt > worst) worst = dt;
    if (dt > tworst) tworst = dt;
    swap_ms += sw; tswap += sw;
    frames++; tframes++;
    double span = (now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    if (g_log_fps && span >= 5.0) {
        rr_log("fps: %.1f (swap interval %d), slowest frame %.0f ms, waiting in swap %.1f ms/frame",
               frames / span, interval, worst, swap_ms / frames);
        start = now; frames = 0; worst = 0; swap_ms = 0;
    }
    double tspan = (now.QuadPart - tstart.QuadPart) / (double)freq.QuadPart;
    if (g_show_fps && tspan >= 0.5) {
        HWND h = rr_game_hwnd();
        char t[128];
        snprintf(t, sizeof t, "RoboRumble - %.0f fps, slowest %.0f ms, swap wait %.1f ms",
                 tframes / tspan, tworst, tswap / tframes);
        /* straight to DefWindowProc: the game's window procedure swallows WM_SETTEXT */
        if (h) DefWindowProcA(h, WM_SETTEXT, 0, (LPARAM)t);
        tstart = now; tframes = 0; tworst = 0; tswap = 0;
    }
}

void glidewin_install(void)
{
    if (strcmp(g_exe_name, "rr_glide.exe") == 0) {
        g_log_fps = GetPrivateProfileIntA("general", "log_fps", 0, g_ini_path);
        g_show_fps = GetPrivateProfileIntA("general", "show_fps", 0, g_ini_path);
        g_swap_override = GetPrivateProfileIntA("window", "glide_swap_interval", 1, g_ini_path);
        if (g_log_fps || g_show_fps || g_swap_override >= 0 || GetPrivateProfileIntA("general", "profile", 0, g_ini_path))
            real_grBufferSwap = rr_hook_iat(GetModuleHandleA(NULL), "glide2x.dll", "_grBufferSwap@4", (void *)fps_grBufferSwap);
    }
    if (rr_nglide_managed()) nglide_no_mode_switch();
    if (strcmp(g_exe_name, "rr_glide.exe") == 0 && !rr_have_dgvoodoo()) texcache_install();
    /* Experiment for the dgVoodoo robot-preview trails; neither a constant nor an alternating
       fill removed them, so it stays off unless [window] shop_black_fix=1. nGlide draws the
       shop correctly. */
    if (GetPrivateProfileIntA("window", "shop_black_fix", 0, g_ini_path)) shop_black_fix();
    if (!g_cfg.window_enabled || strcmp(g_exe_name, "rr_glide.exe") != 0) return;
    if (rr_have_dgvoodoo()) return;   /* dgVoodoo starts windowed by itself */
    if (rr_nglide_managed()) return;  /* nGlide kept windowed by the patch: no Alt+Enter needed */
    if (!GetPrivateProfileIntA("window", "glide_auto_windowed", 1, g_ini_path)) {
        rr_log("glide: auto windowed off (Alt+Enter still switches)");
        return;
    }
    HMODULE exe = GetModuleHandleA(NULL);
    real_grSstWinOpen = rr_hook_iat(exe, "glide2x.dll", "_grSstWinOpen@28", (void *)hook_grSstWinOpen);
    real_CreateWindowExA = rr_hook_iat(exe, "USER32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
    rr_log("glide: windowed hook %s", real_grSstWinOpen ? "installed" : "NOT installed (import missing)");
}
