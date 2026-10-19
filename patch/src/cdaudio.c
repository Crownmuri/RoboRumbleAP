/* CD audio emulation.

   The game plays its soundtrack as Red Book tracks through the MCI "cdaudio" device:
     open -> set time format TMSF -> status length of track N -> play from N:00:00 to N:mm:ss-1
     with MCI_NOTIFY, and on MM_MCINOTIFY(MCI_NOTIFY_SUCCESSFUL) it plays the next/same track.
   Music volume goes through the mixer's compact-disc line (volume control set to the
   in-game music percentage).

   Here "cdaudio" opens a fake device that plays <music folder>\TrackNN.ogg, and the mixer's
   CD line is a fake line whose volume scales that playback. Everything else goes to the real
   winmm. */
#include "common.h"
#include "gen/winmm_table.h"
#include <math.h>
#include <stdio.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

extern void *rr_winmm_ptrs[RR_WINMM_COUNT];

#define FAKE_DEVICE   0xBEEFu
#define FAKE_LINE     0x0CD00000u
#define FAKE_CONTROL  0x0CD00001u
#define MAX_TRACKS    99
#define DATA_TRACK_SECONDS 2.0   /* track 1 is the game's data track; only matters for MSF math */

/* real winmm functions */
static MMRESULT (WINAPI *p_waveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *p_waveOutClose)(HWAVEOUT);
static MMRESULT (WINAPI *p_waveOutPrepareHeader)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *p_waveOutUnprepareHeader)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *p_waveOutWrite)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *p_waveOutReset)(HWAVEOUT);
static MMRESULT (WINAPI *p_mixerGetLineInfoA)(HMIXEROBJ, LPMIXERLINEA, DWORD);
static MMRESULT (WINAPI *p_mixerGetLineControlsA)(HMIXEROBJ, LPMIXERLINECONTROLSA, DWORD);
static MMRESULT (WINAPI *p_mixerGetControlDetailsA)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
static MMRESULT (WINAPI *p_mixerSetControlDetails)(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);

/* ---- disc layout ---- */
static char   g_track_path[MAX_TRACKS + 1][MAX_PATH];
static double g_track_len[MAX_TRACKS + 1];   /* seconds, <0 = not measured yet */
static int    g_num_tracks;                  /* highest track number present */

static double track_length(int t)
{
    if (t < 1 || t > g_num_tracks) return 0;
    if (t == 1 && !g_track_path[1][0]) return DATA_TRACK_SECONDS;
    if (g_track_len[t] < 0) {
        int err;
        g_track_len[t] = 0;
        if (g_track_path[t][0]) {
            stb_vorbis *v = stb_vorbis_open_filename(g_track_path[t], &err, NULL);
            if (v) {
                g_track_len[t] = stb_vorbis_stream_length_in_seconds(v);
                stb_vorbis_close(v);
            }
        }
        if (g_track_len[t] <= 0) g_track_len[t] = 1.0; /* missing track: one silent second */
    }
    return g_track_len[t];
}

static double track_start(int t)
{
    double s = 0;
    for (int i = 1; i < t; i++) s += track_length(i);
    return s;
}

static void scan_tracks(void)
{
    char dir[MAX_PATH];
    if (g_cfg.music_folder[0] && g_cfg.music_folder[1] == ':')
        lstrcpynA(dir, g_cfg.music_folder, MAX_PATH);
    else
        snprintf(dir, sizeof dir, "%s%s", g_game_dir, g_cfg.music_folder);
    g_num_tracks = 0;
    for (int t = 1; t <= MAX_TRACKS; t++) {
        g_track_len[t] = -1;
        g_track_path[t][0] = 0;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\Track%02d.ogg", dir, t);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            lstrcpynA(g_track_path[t], p, MAX_PATH);
            g_num_tracks = t;
        }
    }
    rr_log("music: folder %s, %d tracks", dir, g_num_tracks);
}

/* ---- time formats ---- */
static DWORD g_time_format = MCI_FORMAT_MSF;

static DWORD msf_from_seconds(double sec)
{
    if (sec < 0) sec = 0;
    DWORD frames = (DWORD)(sec * 75.0 + 0.5);
    return MCI_MAKE_MSF(frames / 4500, (frames / 75) % 60, frames % 75);
}

/* position in the current format -> (track, seconds into that track) */
static void decode_position(DWORD v, int *track, double *sec)
{
    double abs;
    switch (g_time_format) {
    case MCI_FORMAT_TMSF:
        *track = MCI_TMSF_TRACK(v);
        *sec = MCI_TMSF_MINUTE(v) * 60.0 + MCI_TMSF_SECOND(v) + MCI_TMSF_FRAME(v) / 75.0;
        return;
    case MCI_FORMAT_MILLISECONDS: abs = v / 1000.0; break;
    default: abs = MCI_MSF_MINUTE(v) * 60.0 + MCI_MSF_SECOND(v) + MCI_MSF_FRAME(v) / 75.0; break;
    }
    int t = 1;
    while (t < g_num_tracks && abs >= track_start(t + 1)) t++;
    *track = t;
    *sec = abs - track_start(t);
}

static DWORD encode_position(int track, double sec)
{
    switch (g_time_format) {
    case MCI_FORMAT_TMSF: {
        DWORD msf = msf_from_seconds(sec);
        return MCI_MAKE_TMSF(track, MCI_MSF_MINUTE(msf), MCI_MSF_SECOND(msf), MCI_MSF_FRAME(msf));
    }
    case MCI_FORMAT_MILLISECONDS: return (DWORD)((track_start(track) + sec) * 1000.0);
    default: return msf_from_seconds(track_start(track) + sec);
    }
}

static DWORD encode_length(double sec)
{
    return g_time_format == MCI_FORMAT_MILLISECONDS ? (DWORD)(sec * 1000.0) : msf_from_seconds(sec);
}

/* ---- playback thread ---- */
static CRITICAL_SECTION g_cs;
static HANDLE g_wake;          /* auto-reset: a new request is waiting */
static HANDLE g_idle;          /* manual-reset: set while the thread is not playing */
static volatile LONG g_abort;
static volatile LONG g_volume = 65535;   /* in-game music volume from the fake mixer control */

static struct {
    int    pending;
    int    track;
    double from, to;           /* seconds into track; to < 0 = end of track */
    HWND   notify;
} g_req;

static int    g_playing;
static int    g_cur_track = 1;
static double g_cur_sec;       /* updated by the thread while playing */
static HWND   g_notify_hwnd;   /* notify target of the running play command */

static void post_notify(HWND hwnd, WPARAM status)
{
    if (hwnd) PostMessageA(hwnd, MM_MCINOTIFY, status, FAKE_DEVICE);
}

/* Some CD rips start with a few milliseconds of full-scale digital noise (GOG's Track02.ogg:
   16 ms at ~46 ms, heard as a loud crack when the main menu appears). Looks at the first
   200 ms for 1 ms windows that are both very loud and noise-like (a zero crossing on more
   than a third of the samples) and returns the sample after the last one, or 0. */
static unsigned skip_rip_noise(int track, int rate, int ch)
{
    static int checked[MAX_TRACKS + 1];
    static unsigned skip[MAX_TRACKS + 1];
    if (checked[track]) return skip[track];
    checked[track] = 1;
    int err;
    stb_vorbis *v = stb_vorbis_open_filename(g_track_path[track], &err, NULL);
    if (!v) return 0;
    int frames = rate / 5, win = rate / 1000;
    short *pcm = malloc(sizeof(short) * frames * ch);
    if (!pcm || win < 8) { free(pcm); stb_vorbis_close(v); return 0; }
    int got = stb_vorbis_get_samples_short_interleaved(v, ch, pcm, frames * ch);
    stb_vorbis_close(v);
    /* silence, then one run of full-scale noise starting in the first 150 ms, at most 50 ms long */
    unsigned last_end = 0, run_start = 0;
    int in_run = 0;
    for (int w = 0; w + win <= got; w += win) {
        int peak = 0, crossings = 0;
        for (int i = w; i < w + win; i++) {
            int s = pcm[i * ch];
            if (abs(s) > peak) peak = abs(s);
            if (i > w && ((s < 0) != (pcm[(i - 1) * ch] < 0))) crossings++;
        }
        int noisy = peak > 28000 && crossings * 3 > win;
        if (noisy && (in_run || w < rate * 150 / 1000)) {
            if (!in_run) run_start = (unsigned)w;
            in_run = 1;
            last_end = (unsigned)(w + win);
            continue;
        }
        if (in_run) break;                       /* the run is over */
        if (peak > 2000) { last_end = 0; break; } /* real audio before any burst: leave the track alone */
    }
    if (last_end - run_start > (unsigned)(rate / 20)) last_end = 0;   /* longer than 50 ms: music, not a glitch */
    if (last_end && !in_run) last_end = 0;
    /* The decoder's first samples don't line up exactly with 1 ms windows, so the tail of the
       burst can fall into a window that no longer counts as noisy. Skip on until the short
       quiet gap that follows the burst (at most 100 ms past its start). */
    if (last_end) {
        unsigned limit = run_start + (unsigned)(rate / 10);
        for (unsigned w = last_end; w + win <= (unsigned)got && w < limit; w += win) {
            int peak = 0;
            for (unsigned i = w; i < w + win; i++) if (abs(pcm[i * ch]) > peak) peak = abs(pcm[i * ch]);
            if (peak < 1500) { last_end = w; break; }
            last_end = w + win;
        }
    }
    free(pcm);
    skip[track] = last_end;
    if (last_end) rr_log("music: track %d starts with %.0f ms of rip noise, skipping it", track, last_end * 1000.0 / rate);
    return last_end;
}

#define BUF_FRAMES 4096
#define NUM_BUFS   4

static void play_track(int track, double from, double to, HWND notify)
{
    int err;
    stb_vorbis *v = g_track_path[track][0] ? stb_vorbis_open_filename(g_track_path[track], &err, NULL) : NULL;
    if (!v) {
        rr_log("music: track %d missing, silent", track);
        Sleep(200);
        if (!g_abort) post_notify(notify, MCI_NOTIFY_SUCCESSFUL);
        return;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int ch = info.channels > 2 ? 2 : info.channels;
    unsigned total = stb_vorbis_stream_length_in_samples(v);
    unsigned start = (unsigned)(from * info.sample_rate);
    if (start == 0) start = skip_rip_noise(track, info.sample_rate, ch);
    unsigned end = to < 0 ? total : (unsigned)(to * info.sample_rate);
    if (end > total) end = total;
    if (start > 0 && start < total) stb_vorbis_seek(v, start);

    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)ch;
    wf.nSamplesPerSec = info.sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(ch * 2);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    HANDLE done_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT wo;
    if (p_waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)done_evt, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        rr_log("music: waveOutOpen failed");
        stb_vorbis_close(v);
        CloseHandle(done_evt);
        return;
    }
    rr_log("music: play track %d %.1f..%.1f s (%d Hz, %d ch)", track, from, end / (double)info.sample_rate,
           info.sample_rate, ch);

    static short pcm[NUM_BUFS][BUF_FRAMES * 2];
    WAVEHDR hdr[NUM_BUFS] = {0};
    unsigned pos = start;
    int finished = 0, queued = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        hdr[i].lpData = (LPSTR)pcm[i];
        hdr[i].dwFlags = WHDR_DONE;
    }
    while (!g_abort) {
        int any_busy = 0;
        for (int i = 0; i < NUM_BUFS && !g_abort; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) { any_busy = 1; continue; }
            if (hdr[i].dwFlags & WHDR_PREPARED) {
                p_waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
                queued--;
            }
            if (finished) continue;
            int want = BUF_FRAMES;
            if (pos + want > end) want = (int)(end - pos);
            int got = want > 0 ? stb_vorbis_get_samples_short_interleaved(v, ch, pcm[i], want * ch) : 0;
            if (got <= 0) { finished = 1; continue; }
            if (pos == start) {   /* 5 ms fade-in: tracks start mid-waveform after a seek or skip */
                int fade = info.sample_rate / 200;
                for (int f = 0; f < fade && f < got; f++)
                    for (int c = 0; c < ch; c++) pcm[i][f * ch + c] = (short)(pcm[i][f * ch + c] * f / fade);
            }
            pos += got;
            double gain = (g_volume / 65535.0) * (g_cfg.music_volume / 100.0);
            if (gain < 0.999) {
                int g = (int)(gain * 32768.0);
                for (int s = 0; s < got * ch; s++) pcm[i][s] = (short)((pcm[i][s] * g) >> 15);
            }
            hdr[i].dwBufferLength = got * ch * 2;
            hdr[i].dwFlags = 0;
            p_waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            p_waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR));
            queued++;
            any_busy = 1;
        }
        EnterCriticalSection(&g_cs);
        g_cur_sec = from + (pos - start) / (double)info.sample_rate;
        LeaveCriticalSection(&g_cs);
        if (finished && !any_busy && queued <= 0) break;
        WaitForSingleObject(done_evt, 50);
    }
    p_waveOutReset(wo);
    for (int i = 0; i < NUM_BUFS; i++)
        if (hdr[i].dwFlags & WHDR_PREPARED) p_waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
    p_waveOutClose(wo);
    CloseHandle(done_evt);
    stb_vorbis_close(v);
    if (!g_abort) {
        rr_log("music: track %d finished", track);
        EnterCriticalSection(&g_cs);
        HWND h = g_notify_hwnd;
        g_notify_hwnd = NULL;
        LeaveCriticalSection(&g_cs);
        post_notify(h, MCI_NOTIFY_SUCCESSFUL);
    }
}

static DWORD WINAPI music_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        EnterCriticalSection(&g_cs);
        if (!g_req.pending) { LeaveCriticalSection(&g_cs); continue; }
        int track = g_req.track;
        double from = g_req.from, to = g_req.to;
        HWND notify = g_req.notify;
        g_req.pending = 0;
        g_playing = 1;
        g_cur_track = track;
        g_cur_sec = from;
        ResetEvent(g_idle);
        InterlockedExchange(&g_abort, 0);
        LeaveCriticalSection(&g_cs);

        play_track(track, from, to, notify);

        EnterCriticalSection(&g_cs);
        g_playing = 0;
        SetEvent(g_idle);
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

/* Stops the current track. A pending notify gets `status` like a real device would send. */
static void stop_playback(WPARAM status)
{
    EnterCriticalSection(&g_cs);
    HWND h = g_notify_hwnd;
    g_notify_hwnd = NULL;
    g_req.pending = 0;
    InterlockedExchange(&g_abort, 1);
    LeaveCriticalSection(&g_cs);
    WaitForSingleObject(g_idle, 2000);
    if (h) post_notify(h, status);
}

static void start_playback(int track, double from, double to, HWND notify)
{
    stop_playback(MCI_NOTIFY_SUPERSEDED);
    if (track < 1) track = 1;
    if (track > g_num_tracks) track = g_num_tracks;
    EnterCriticalSection(&g_cs);
    g_req.pending = 1;
    g_req.track = track;
    g_req.from = from;
    g_req.to = to;
    g_req.notify = notify;
    g_notify_hwnd = notify;
    g_cur_track = track;
    g_cur_sec = from;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
}

/* ---- MCI ---- */
static MCIERROR fake_command(UINT msg, DWORD_PTR flags, DWORD_PTR param)
{
    switch (msg) {
    case MCI_CLOSE:
        stop_playback(MCI_NOTIFY_ABORTED);
        rr_log("mci: close");
        return 0;
    case MCI_SET: {
        MCI_SET_PARMS *p = (MCI_SET_PARMS *)param;
        if ((flags & MCI_SET_TIME_FORMAT) && p) g_time_format = p->dwTimeFormat;
        return 0;
    }
    case MCI_STATUS: {
        MCI_STATUS_PARMS *p = (MCI_STATUS_PARMS *)param;
        if (!p || !(flags & MCI_STATUS_ITEM)) return MCIERR_MISSING_PARAMETER;
        int track = (flags & MCI_TRACK) ? (int)p->dwTrack : 0;
        switch (p->dwItem) {
        case MCI_STATUS_LENGTH:
            p->dwReturn = encode_length(track ? track_length(track) : track_start(g_num_tracks + 1));
            break;
        case MCI_STATUS_POSITION:
            if (track) p->dwReturn = encode_position(track, 0);
            else {
                EnterCriticalSection(&g_cs);
                p->dwReturn = encode_position(g_cur_track, g_cur_sec);
                LeaveCriticalSection(&g_cs);
            }
            break;
        case MCI_STATUS_NUMBER_OF_TRACKS: p->dwReturn = g_num_tracks; break;
        case MCI_STATUS_MODE: p->dwReturn = g_playing || g_req.pending ? MCI_MODE_PLAY : MCI_MODE_STOP; break;
        case MCI_STATUS_MEDIA_PRESENT:
        case MCI_STATUS_READY: p->dwReturn = TRUE; break;
        case MCI_STATUS_TIME_FORMAT: p->dwReturn = g_time_format; break;
        case MCI_STATUS_CURRENT_TRACK: p->dwReturn = g_cur_track; break;
        case MCI_CDA_STATUS_TYPE_TRACK:
            p->dwReturn = (track == 1 && !g_track_path[1][0]) ? MCI_CDA_TRACK_OTHER : MCI_CDA_TRACK_AUDIO;
            break;
        default: p->dwReturn = 0; break;
        }
        return 0;
    }
    case MCI_PLAY: {
        MCI_PLAY_PARMS *p = (MCI_PLAY_PARMS *)param;
        int track;
        double from, to = -1;
        if (p && (flags & MCI_FROM)) decode_position(p->dwFrom, &track, &from);
        else {
            EnterCriticalSection(&g_cs);
            track = g_cur_track;
            from = g_cur_sec;
            LeaveCriticalSection(&g_cs);
        }
        if (p && (flags & MCI_TO)) {
            int to_track;
            double to_sec;
            decode_position(p->dwTo, &to_track, &to_sec);
            if (to_track == track) to = to_sec;
        }
        if (track == 1 && !g_track_path[1][0] && g_num_tracks >= 2) { track = 2; from = 0; }
        start_playback(track, from, to, (p && (flags & MCI_NOTIFY)) ? (HWND)p->dwCallback : NULL);
        return 0;
    }
    case MCI_STOP:
    case MCI_PAUSE:
        stop_playback(MCI_NOTIFY_ABORTED);
        if ((flags & MCI_NOTIFY) && param)
            post_notify((HWND)((MCI_GENERIC_PARMS *)param)->dwCallback, MCI_NOTIFY_SUCCESSFUL);
        return 0;
    case MCI_RESUME:
        start_playback(g_cur_track, g_cur_sec, -1, NULL);
        return 0;
    case MCI_SEEK: {
        MCI_SEEK_PARMS *p = (MCI_SEEK_PARMS *)param;
        stop_playback(MCI_NOTIFY_ABORTED);
        if (flags & MCI_SEEK_TO_START) { g_cur_track = 1; g_cur_sec = 0; }
        else if (flags & MCI_SEEK_TO_END) { g_cur_track = g_num_tracks; g_cur_sec = track_length(g_num_tracks); }
        else if (p && (flags & MCI_TO)) decode_position(p->dwTo, &g_cur_track, &g_cur_sec);
        return 0;
    }
    case MCI_GETDEVCAPS: {
        MCI_GETDEVCAPS_PARMS *p = (MCI_GETDEVCAPS_PARMS *)param;
        if (!p) return 0;
        p->dwReturn = p->dwItem == MCI_GETDEVCAPS_DEVICE_TYPE ? MCI_DEVTYPE_CD_AUDIO
                    : (p->dwItem == MCI_GETDEVCAPS_CAN_RECORD || p->dwItem == MCI_GETDEVCAPS_CAN_SAVE ||
                       p->dwItem == MCI_GETDEVCAPS_USES_FILES || p->dwItem == MCI_GETDEVCAPS_COMPOUND_DEVICE)
                          ? FALSE : TRUE;
        return 0;
    }
    default:
        return 0;
    }
}

static int is_cdaudio_open(DWORD_PTR flags, DWORD_PTR param)
{
    MCI_OPEN_PARMSA *p = (MCI_OPEN_PARMSA *)param;
    if (!p || !(flags & MCI_OPEN_TYPE)) return 0;
    if (flags & MCI_OPEN_TYPE_ID) return LOWORD((DWORD_PTR)p->lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO;
    return p->lpstrDeviceType && lstrcmpiA(p->lpstrDeviceType, "cdaudio") == 0;
}

static MCIERROR WINAPI hook_mciSendCommandA(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR param)
{
    if (msg == MCI_OPEN && is_cdaudio_open(flags, param)) {
        ((MCI_OPEN_PARMSA *)param)->wDeviceID = FAKE_DEVICE;
        g_time_format = MCI_FORMAT_MSF;
        rr_log("mci: open cdaudio -> fake device");
        return 0;
    }
    if (id == FAKE_DEVICE) return fake_command(msg, flags, param);
    if (id == MCI_ALL_DEVICE_ID && msg == MCI_CLOSE) stop_playback(MCI_NOTIFY_ABORTED);
    return real_mciSendCommandA(id, msg, flags, param);
}

/* ---- mixer: fake compact-disc line carrying the music volume ---- */
static MMRESULT WINAPI hook_mixerGetLineInfoA(HMIXEROBJ h, LPMIXERLINEA line, DWORD flags)
{
    if (line && (flags & MIXER_GETLINEINFOF_QUERYMASK) == MIXER_GETLINEINFOF_COMPONENTTYPE &&
        line->dwComponentType == MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC) {
        DWORD type = line->dwComponentType;
        memset(line, 0, sizeof *line);
        line->cbStruct = sizeof *line;
        line->dwLineID = FAKE_LINE;
        line->dwComponentType = type;
        line->cChannels = 2;
        line->cControls = 1;
        line->fdwLine = MIXERLINE_LINEF_ACTIVE | MIXERLINE_LINEF_SOURCE;
        lstrcpyA(line->szShortName, "CD");
        lstrcpyA(line->szName, "CD Audio (RoboRumble patch)");
        return MMSYSERR_NOERROR;
    }
    return p_mixerGetLineInfoA(h, line, flags);
}

static MMRESULT WINAPI hook_mixerGetLineControlsA(HMIXEROBJ h, LPMIXERLINECONTROLSA lc, DWORD flags)
{
    if (lc && lc->dwLineID == FAKE_LINE && lc->pamxctrl && lc->cbmxctrl >= sizeof(MIXERCONTROLA)) {
        MIXERCONTROLA *c = lc->pamxctrl;
        memset(c, 0, sizeof *c);
        c->cbStruct = sizeof *c;
        c->dwControlID = FAKE_CONTROL;
        c->dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
        c->Bounds.dwMinimum = 0;
        c->Bounds.dwMaximum = 65535;
        c->Metrics.cSteps = 65536;
        lstrcpyA(c->szShortName, "Volume");
        lstrcpyA(c->szName, "CD Volume");
        lc->cControls = 1;
        return MMSYSERR_NOERROR;
    }
    return p_mixerGetLineControlsA(h, lc, flags);
}

static MMRESULT WINAPI hook_mixerGetControlDetailsA(HMIXEROBJ h, LPMIXERCONTROLDETAILS d, DWORD flags)
{
    if (d && d->dwControlID == FAKE_CONTROL && d->paDetails && d->cbDetails >= 4) {
        DWORD n = d->cChannels ? d->cChannels : 1;
        for (DWORD i = 0; i < n; i++) ((DWORD *)d->paDetails)[i] = (DWORD)g_volume;
        return MMSYSERR_NOERROR;
    }
    return p_mixerGetControlDetailsA(h, d, flags);
}

static MMRESULT WINAPI hook_mixerSetControlDetails(HMIXEROBJ h, LPMIXERCONTROLDETAILS d, DWORD flags)
{
    if (d && d->dwControlID == FAKE_CONTROL && d->paDetails && d->cbDetails >= 4) {
        DWORD v = ((DWORD *)d->paDetails)[0];
        if (v > 65535) v = 65535;
        InterlockedExchange(&g_volume, (LONG)v);
        rr_log("music: volume %lu/65535", v);
        return MMSYSERR_NOERROR;
    }
    return p_mixerSetControlDetails(h, d, flags);
}

/* Fallback when the game never sets the mixer (no mixer devices): the launcher's MusicVolume. */
static void initial_volume(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%srrumble.ini", g_game_dir);
    int v = (int)GetPrivateProfileIntA("config", "MusicVolume", 100, path);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_volume = v * 65535 / 100;
}

void cd_install(void)
{
    if (!g_cfg.music_enabled) { rr_log("music: disabled in rrpatch.ini"); return; }
    scan_tracks();
    if (g_num_tracks < 2) { rr_log("music: no TrackNN.ogg files found, leaving cdaudio alone"); return; }
    p_waveOutOpen = (void *)rr_real("waveOutOpen");
    p_waveOutClose = (void *)rr_real("waveOutClose");
    p_waveOutPrepareHeader = (void *)rr_real("waveOutPrepareHeader");
    p_waveOutUnprepareHeader = (void *)rr_real("waveOutUnprepareHeader");
    p_waveOutWrite = (void *)rr_real("waveOutWrite");
    p_waveOutReset = (void *)rr_real("waveOutReset");
    p_mixerGetLineInfoA = (void *)rr_real("mixerGetLineInfoA");
    p_mixerGetLineControlsA = (void *)rr_real("mixerGetLineControlsA");
    p_mixerGetControlDetailsA = (void *)rr_real("mixerGetControlDetailsA");
    p_mixerSetControlDetails = (void *)rr_real("mixerSetControlDetails");
    initial_volume();

    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_idle = CreateEventA(NULL, TRUE, TRUE, NULL);
    CloseHandle(CreateThread(NULL, 0, music_thread, NULL, 0, NULL));

    rr_winmm_ptrs[RRW_mciSendCommandA] = (void *)hook_mciSendCommandA;
    rr_winmm_ptrs[RRW_mixerGetLineInfoA] = (void *)hook_mixerGetLineInfoA;
    rr_winmm_ptrs[RRW_mixerGetLineControlsA] = (void *)hook_mixerGetLineControlsA;
    rr_winmm_ptrs[RRW_mixerGetControlDetailsA] = (void *)hook_mixerGetControlDetailsA;
    rr_winmm_ptrs[RRW_mixerSetControlDetails] = (void *)hook_mixerSetControlDetails;
    rr_log("music: cdaudio emulation active");
}
