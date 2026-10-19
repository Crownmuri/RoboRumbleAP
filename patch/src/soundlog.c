/* [general] log_sound=1: logs what the game does with DirectSound (buffer creation, Play,
   Stop, volume/pan/frequency changes) to find where audio glitches come from. Wraps the
   vtable entries of the real objects; behaviour is unchanged (off by default). */
#include "common.h"
#include <dsound.h>
#include <stdio.h>

typedef HRESULT (WINAPI *DirectSoundCreate_t)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
static DirectSoundCreate_t real_DirectSoundCreate;


static HRESULT STDMETHODCALLTYPE log_Lock(IDirectSoundBuffer *, DWORD, DWORD, LPVOID *, LPDWORD, LPVOID *, LPDWORD, DWORD);

/* the last volume/pan per buffer, to log only changes */
#define TRACK 256
static struct { void *buf; LONG vol, pan; DWORD freq; int playing; } g_track[TRACK];

static int slot(void *buf)
{
    for (int i = 0; i < TRACK; i++) if (g_track[i].buf == buf) return i;
    for (int i = 0; i < TRACK; i++) if (!g_track[i].buf) { g_track[i].buf = buf; g_track[i].vol = 1; g_track[i].pan = 1; return i; }
    return 0;
}

/* DirectSound uses different method tables for different kinds of objects (the primary
   buffer's differs from normal buffers), so the original methods are kept per table and
   every wrapper calls the original of the table its object actually uses. */
#define MAXVT 16
static struct { void **vt; void *orig[24]; } g_vts[MAXVT];
static int g_nvt;
static CRITICAL_SECTION g_vt_cs;

static void *orig_of(void *obj, int index)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < g_nvt; i++)
        if (g_vts[i].vt == vt) return g_vts[i].orig[index];
    return NULL;
}

static void patch_vtbl(void **vtbl, int index, void *fn)
{
    EnterCriticalSection(&g_vt_cs);
    int e = 0;
    while (e < g_nvt && g_vts[e].vt != vtbl) e++;
    if (e == g_nvt) {
        if (g_nvt == MAXVT) { LeaveCriticalSection(&g_vt_cs); return; }
        g_vts[g_nvt++].vt = vtbl;
    }
    if (vtbl[index] != fn) {
        DWORD old;
        g_vts[e].orig[index] = vtbl[index];
        VirtualProtect(&vtbl[index], sizeof(void *), PAGE_READWRITE, &old);
        vtbl[index] = fn;
        VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    }
    LeaveCriticalSection(&g_vt_cs);
}

typedef HRESULT (STDMETHODCALLTYPE *Play_t)(IDirectSoundBuffer *, DWORD, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *Unary_t)(IDirectSoundBuffer *);
typedef HRESULT (STDMETHODCALLTYPE *Long_t)(IDirectSoundBuffer *, LONG);
typedef HRESULT (STDMETHODCALLTYPE *Dword_t)(IDirectSoundBuffer *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *Format_t)(IDirectSoundBuffer *, LPCWAVEFORMATEX);
typedef HRESULT (STDMETHODCALLTYPE *Lock_t)(IDirectSoundBuffer *, DWORD, DWORD, LPVOID *, LPDWORD, LPVOID *, LPDWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *Create_t)(IDirectSound *, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);

static HRESULT STDMETHODCALLTYPE log_Play(IDirectSoundBuffer *b, DWORD r1, DWORD prio, DWORD flags)
{
    int s = slot(b);
    LONG vol = 0, pan = 0;
    DWORD freq = 0, playc = 0, writec = 0;
    b->lpVtbl->GetVolume(b, &vol);
    b->lpVtbl->GetPan(b, &pan);
    b->lpVtbl->GetFrequency(b, &freq);
    b->lpVtbl->GetCurrentPosition(b, &playc, &writec);
    rr_log("snd: Play   buf %p%s vol %ld pan %ld freq %lu pos %lu", b, flags & DSBPLAY_LOOPING ? " looping" : "",
           vol, pan, freq, playc);
    g_track[s].playing = 1;
    return ((Play_t)orig_of(b, 12))(b, r1, prio, flags);
}

static HRESULT STDMETHODCALLTYPE log_Stop(IDirectSoundBuffer *b)
{
    int s = slot(b);
    if (g_track[s].playing) rr_log("snd: Stop   buf %p", b);
    g_track[s].playing = 0;
    return ((Unary_t)orig_of(b, 18))(b);
}

static HRESULT STDMETHODCALLTYPE log_SetVolume(IDirectSoundBuffer *b, LONG v)
{
    int s = slot(b);
    HRESULT hr = ((Long_t)orig_of(b, 15))(b, v);
    if (g_track[s].vol != v || FAILED(hr)) {
        if (g_track[s].playing || g_track[s].vol == 1 || FAILED(hr)) {
            LONG now = 0;
            b->lpVtbl->GetVolume(b, &now);
            rr_log("snd: Volume buf %p %ld -> %08lx (now %ld)%s", b, v, (DWORD)hr, now, g_track[s].playing ? " (while playing)" : "");
        }
        g_track[s].vol = v;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE log_SetPan(IDirectSoundBuffer *b, LONG p)
{
    int s = slot(b);
    if (g_track[s].pan != p && (g_track[s].pan == 1 || p - g_track[s].pan > 2000 || g_track[s].pan - p > 2000))
        rr_log("snd: Pan    buf %p %ld", b, p);
    g_track[s].pan = p;
    return ((Long_t)orig_of(b, 16))(b, p);
}

static HRESULT STDMETHODCALLTYPE log_SetFrequency(IDirectSoundBuffer *b, DWORD f)
{
    int s = slot(b);
    if (g_track[s].freq != f) rr_log("snd: Freq   buf %p %lu", b, f);
    g_track[s].freq = f;
    return ((Dword_t)orig_of(b, 17))(b, f);
}

static HRESULT STDMETHODCALLTYPE log_SetFormat(IDirectSoundBuffer *b, LPCWAVEFORMATEX wf)
{
    HRESULT hr = ((Format_t)orig_of(b, 14))(b, wf);
    if (wf) rr_log("snd: Format buf %p %lu Hz %u ch %u bit -> %08lx", b, wf->nSamplesPerSec, wf->nChannels, wf->wBitsPerSample, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE log_SetCurrentPosition(IDirectSoundBuffer *b, DWORD pos)
{
    if (g_track[slot(b)].playing) rr_log("snd: Seek   buf %p %lu (while playing)", b, pos);
    return ((Dword_t)orig_of(b, 13))(b, pos);
}

static void hook_buffer(IDirectSoundBuffer *b)
{
    void **vt = *(void ***)b;
    patch_vtbl(vt, 12, (void *)log_Play);
    patch_vtbl(vt, 13, (void *)log_SetCurrentPosition);
    patch_vtbl(vt, 14, (void *)log_SetFormat);
    patch_vtbl(vt, 15, (void *)log_SetVolume);
    patch_vtbl(vt, 16, (void *)log_SetPan);
    patch_vtbl(vt, 17, (void *)log_SetFrequency);
    patch_vtbl(vt, 18, (void *)log_Stop);
    patch_vtbl(vt, 11, (void *)log_Lock);
}

static HRESULT STDMETHODCALLTYPE log_CreateSoundBuffer(IDirectSound *ds, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN u)
{
    HRESULT hr = ((Create_t)orig_of(ds, 3))(ds, d, out, u);
    if (d) {
        const WAVEFORMATEX *wf = d->lpwfxFormat;
        rr_log("snd: Create buf %p flags %08lx bytes %lu %lu Hz %u ch %u bit -> %08lx", out ? *out : NULL, d->dwFlags,
               d->dwBufferBytes, wf ? wf->nSamplesPerSec : 0, wf ? wf->nChannels : 0, wf ? wf->wBitsPerSample : 0, hr);
    }
    if (SUCCEEDED(hr) && out && *out) hook_buffer(*out);
    return hr;
}

static HRESULT WINAPI log_DirectSoundCreate(LPCGUID g, LPDIRECTSOUND *out, LPUNKNOWN u)
{
    HRESULT hr = real_DirectSoundCreate(g, out, u);
    rr_log("snd: DirectSoundCreate -> %08lx", hr);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*(void ***)*out, 3, (void *)log_CreateSoundBuffer);
    return hr;
}

/* The video player reads audio and frames with AVIStreamRead and never looks at the message
   queue, so windowed games got flagged "Not Responding" during videos. Peeking (without
   removing anything) on every read keeps the window alive on every renderer. */
typedef LONG (WINAPI *AVIStreamRead_t)(void *, LONG, LONG, LPVOID, LONG, LONG *, LONG *);
static AVIStreamRead_t real_AVIStreamRead;
static int g_log_sound;

static LONG WINAPI hook_AVIStreamRead(void *st, LONG start, LONG n, LPVOID buf, LONG bytes, LONG *rb, LONG *rs)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last > 50) {
        MSG m;
        PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE);
        last = now;
    }
    LONG hr = real_AVIStreamRead(st, start, n, buf, bytes, rb, rs);
    static int logged;
    if (g_log_sound && (hr != 0 || (buf && bytes > 0 && n > 1 && logged < 12))) {
        rr_log("avi: read stream %p start %ld count %ld bytes %ld -> %08lx", st, start, n, bytes, (DWORD)hr);
        logged++;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE log_Lock(IDirectSoundBuffer *b, DWORD off, DWORD bytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD flags)
{
    HRESULT hr = ((Lock_t)orig_of(b, 11))(b, off, bytes, p1, n1, p2, n2, flags);
    static int logged;
    if (bytes > 4096 && logged < 12) {
        DWORD play = 0, write = 0;
        b->lpVtbl->GetCurrentPosition(b, &play, &write);
        rr_log("snd: Lock   buf %p off %lu bytes %lu (play %lu write %lu) -> %08lx", b, off, bytes, play, write, hr);
        logged++;
    }
    return hr;
}

void soundlog_install(void)
{
    real_AVIStreamRead = rr_hook_iat(GetModuleHandleA(NULL), "AVIFIL32.dll", "AVIStreamRead", (void *)hook_AVIStreamRead);
    g_log_sound = GetPrivateProfileIntA("general", "log_sound", 0, g_ini_path);
    if (!g_log_sound) return;
    InitializeCriticalSection(&g_vt_cs);
    real_DirectSoundCreate = rr_hook_iat(GetModuleHandleA(NULL), "DSOUND.dll", "DirectSoundCreate", (void *)log_DirectSoundCreate);
    rr_log("snd: logging %s", real_DirectSoundCreate ? "on" : "failed (import not found)");
}
