/* [general] profile=1 (diagnostic): a sampling profiler for the game's main thread.
   A background thread stops the game thread about 1000 times a second, notes where it was
   (which module, and for the exe which 256-byte block of code) and lets it go again.
   Every 5 s the log gets the share of time per module (game exe, nGlide, Direct3D, driver,
   Windows) and the busiest blocks of game code, as milliseconds per frame.
   The game thread is the one that calls grBufferSwap (prof_frame). */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define EXE_BINS 0x4000          /* 4 MB of exe image in 256-byte blocks */
#define MAX_MODS 64

static HANDLE g_thread;
static volatile LONG g_frames;
static DWORD g_exe_bins[EXE_BINS];
static struct { HMODULE mod; char name[48]; DWORD count; } g_mods[MAX_MODS];
static int g_nmods;
static DWORD g_other, g_total;
#define GL_BINS 0x2000            /* 2 MB of glide2x.dll in 256-byte blocks */
static DWORD g_gl_bins[GL_BINS];
static BYTE *g_gl_base;

static void add_sample(DWORD eip)
{
    g_total++;
    if (eip >= 0x400000 && eip < 0x400000 + EXE_BINS * 256) { g_exe_bins[(eip - 0x400000) >> 8]++; return; }
    if (g_gl_base && eip >= (DWORD)g_gl_base && eip < (DWORD)g_gl_base + GL_BINS * 256)
        g_gl_bins[(eip - (DWORD)g_gl_base) >> 8]++;
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)eip, &m) || !m) { g_other++; return; }
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].mod == m) { g_mods[i].count++; return; }
    if (g_nmods == MAX_MODS) { g_other++; return; }
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, sizeof path);
    const char *n = strrchr(path, '\\');
    g_mods[g_nmods].mod = m;
    lstrcpynA(g_mods[g_nmods].name, n ? n + 1 : path, sizeof g_mods[0].name);
    g_mods[g_nmods++].count = 1;
}

static void report(double secs)
{
    LONG frames = InterlockedExchange(&g_frames, 0);
    if (!g_total || frames <= 0) return;
    double ms_per_sample = secs * 1000.0 / g_total;     /* wall time each sample stands for */
    double f = ms_per_sample / frames;                   /* -> ms per frame */
    DWORD exe = 0;
    for (int i = 0; i < EXE_BINS; i++) exe += g_exe_bins[i];
    char line[512];
    int len = snprintf(line, sizeof line, "prof: %.1f fps, ms/frame: exe %.1f", frames / secs, exe * f);
    /* modules, biggest first */
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i < g_nmods; i++)
            if (g_mods[i].count && (best < 0 || g_mods[i].count > g_mods[best].count)) best = i;
        if (best < 0) break;
        len += snprintf(line + len, sizeof line - len, ", %s %.1f", g_mods[best].name, g_mods[best].count * f);
        g_mods[best].count = 0;
    }
    DWORD rest = g_other;
    for (int i = 0; i < g_nmods; i++) { rest += g_mods[i].count; g_mods[i].count = 0; }
    snprintf(line + len, sizeof line - len, ", rest %.1f", rest * f);
    rr_log("%s", line);
    /* hottest exe blocks */
    len = snprintf(line, sizeof line, "prof:   exe hot:");
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < EXE_BINS; i++)
            if (g_exe_bins[i] && (best < 0 || g_exe_bins[i] > g_exe_bins[best])) best = i;
        if (best < 0) break;
        len += snprintf(line + len, sizeof line - len, " %06X:%.1f", 0x400000 + best * 256, g_exe_bins[best] * f);
        g_exe_bins[best] = 0;
    }
    rr_log("%s", line);
    len = snprintf(line, sizeof line, "prof:   glide2x hot (offset):");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < GL_BINS; i++)
            if (g_gl_bins[i] && (best < 0 || g_gl_bins[i] > g_gl_bins[best])) best = i;
        if (best < 0) break;
        len += snprintf(line + len, sizeof line - len, " %05X:%.1f", best * 256, g_gl_bins[best] * f);
        g_gl_bins[best] = 0;
    }
    rr_log("%s", line);
    memset(g_gl_bins, 0, sizeof g_gl_bins);
    memset(g_exe_bins, 0, sizeof g_exe_bins);
    g_other = g_total = 0;
}

static DWORD WINAPI sampler(LPVOID unused)
{
    (void)unused;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (;;) {
        Sleep(1);
        if (SuspendThread(g_thread) == (DWORD)-1) return 0;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_thread, &c)) add_sample(c.Eip);
        ResumeThread(g_thread);
        QueryPerformanceCounter(&now);
        double secs = (now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (secs >= 5.0) { report(secs); start = now; }
    }
}

void prof_frame(void)
{
    static int state;   /* 0 = not checked, 1 = running, 2 = off */
    if (state == 0) {
        state = 2;
        if (!GetPrivateProfileIntA("general", "profile", 0, g_ini_path)) return;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) return;
        HANDLE t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
        if (!t) return;
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
        g_gl_base = (BYTE *)GetModuleHandleA("glide2x.dll");
        state = 1;
        rr_log("prof: sampling the game thread");
    }
    if (state == 1) InterlockedIncrement(&g_frames);
}
