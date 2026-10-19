/* [window] glide_ncc_cache=1 (rr_glide.exe): fast YIQ textures on nGlide.

   Most of the game's textures use the Voodoo's YIQ ("NCC") compressed format: 8 bits per
   texel (4 Y, 2 I, 2 Q) decoded through a small colour table that the game uploads per
   texture (grTexDownloadTable). The game draws the map in map order, so it switches texture
   and table every few triangles; zoomed out that is 1500+ switches per frame, and nGlide
   decodes the whole texture again on the CPU on every switch (most of a 30-40 ms frame).

   The patch keeps the game's YIQ textures and tables itself, decodes each (texture, table)
   pair once into a 16-bit texture and uploads that to the upper part of texture memory,
   which is hidden from the game (grTexMaxAddress reports less). grTexSource then binds the
   decoded copy, and nGlide never sees an NCC table. A (re)download of a texture by the game
   invalidates its decoded copies; when the reserved memory is full it is simply emptied. */
#include "common.h"
#include <stdlib.h>

typedef struct { int smallLod, largeLod, aspect, format; void *data; } GrTexInfo;

#define FMT_YIQ_422     0x1
#define FMT_AYIQ_8422   0x9
#define FMT_RGB_565     0xa
#define FMT_ARGB_4444   0xc
#define TABLE_NCC0      0
#define TABLE_NCC1      1

static void (__stdcall *real_DownloadMipMap)(int, DWORD, DWORD, GrTexInfo *);
static void (__stdcall *real_DownloadTable)(int, int, void *);
static void (__stdcall *real_NCCTable)(int, int);
static void (__stdcall *real_TexSource)(int, DWORD, DWORD, GrTexInfo *);
static DWORD (__stdcall *real_MaxAddress)(int);
static DWORD (__stdcall *real_MinAddress)(int);
static DWORD (__stdcall *real_MemRequired)(DWORD, GrTexInfo *);

/* ---------------------------------------------------------------- NCC tables */
/* GuNccTable: yRGB[16] bytes, iRGB[4][3] and qRGB[4][3] shorts, packed_data[12] dwords */
#define NCC_SIZE 112
typedef struct { BYTE raw[NCC_SIZE]; DWORD crc; int sent; } Ncc;
static Ncc g_ncc[2][2];               /* [tmu][table] */
static int g_ncc_active[2];

static DWORD crc_bytes(const BYTE *p, int n)
{
    DWORD h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ---------------------------------------------------------------- the game's YIQ textures */
typedef struct {
    int tmu;
    DWORD addr, end, evenOdd;
    GrTexInfo info;
    BYTE *data;
    DWORD gen;                        /* changes on every download into this record */
} Src;
#define MAX_SRC 1024
static Src g_src[MAX_SRC];
static int g_nsrc;
static DWORD g_gen;

static void lod_size(int lod, int aspect, int *w, int *h)
{
    int s = 256 >> lod;
    *w = *h = s;
    if (aspect < 3) { *h = s >> (3 - aspect); if (*h < 1) *h = 1; }
    if (aspect > 3) { *w = s >> (aspect - 3); if (*w < 1) *w = 1; }
}

static int chain_texels(const GrTexInfo *t)
{
    int n = 0, w, h;
    for (int lod = t->largeLod; lod <= t->smallLod; lod++) { lod_size(lod, t->aspect, &w, &h); n += w * h; }
    return n;
}

static Src *find_src(int tmu, DWORD addr, DWORD evenOdd)
{
    for (int i = 0; i < g_nsrc; i++)
        if (g_src[i].tmu == tmu && g_src[i].addr == addr && g_src[i].evenOdd == evenOdd) return &g_src[i];
    return NULL;
}

/* ---------------------------------------------------------------- decoded copies */
typedef struct { int used; int tmu; DWORD addr, evenOdd, gen, crc, dst; GrTexInfo info; } Entry;
#define MAX_ENTRY 4096
static Entry g_cache[MAX_ENTRY];
static DWORD g_area_lo[2], g_area_hi[2], g_area_next[2];
static int g_flushes, g_decodes;

static void flush(int tmu)
{
    /* both TMUs: an open-addressed table can't lose single entries */
    (void)tmu;
    memset(g_cache, 0, sizeof g_cache);
    g_area_next[0] = g_area_lo[0];
    g_area_next[1] = g_area_lo[1];
    g_flushes++;
}

static DWORD alloc_area(int tmu, DWORD size)
{
    DWORD a = (g_area_next[tmu] + 7) & ~7u;
    if ((a >> 21) != ((a + size - 1) >> 21)) a = (a + size - 1) & ~0x1FFFFFu;   /* no 2 MB crossing */
    if (a + size > g_area_hi[tmu]) return 0;
    g_area_next[tmu] = a + size;
    return a;
}

static Entry *lookup(int tmu, DWORD addr, DWORD evenOdd, DWORD gen, DWORD crc)
{
    DWORD h = (addr * 2654435761u) ^ crc ^ (evenOdd << 3) ^ tmu;
    for (int i = 0; i < MAX_ENTRY; i++) {
        Entry *e = &g_cache[(h + i) % MAX_ENTRY];
        if (!e->used) return e;
        if (e->tmu == tmu && e->addr == addr && e->evenOdd == evenOdd && e->gen == gen && e->crc == crc) return e;
    }
    return NULL;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void decode(const Src *s, const Ncc *t, WORD *out)
{
    const DWORD *packed = (const DWORD *)(t->raw + 64);
    const BYTE *y = (const BYTE *)packed;             /* 16 Y values, 4 per dword */
    int ir[4], ig[4], ib[4], qr[4], qg[4], qb[4];
    for (int k = 0; k < 4; k++) {
        DWORD i = packed[4 + k], q = packed[8 + k];
        ir[k] = (int)(i << 5) >> 23; ig[k] = (int)(i << 14) >> 23; ib[k] = (int)(i << 23) >> 23;
        qr[k] = (int)(q << 5) >> 23; qg[k] = (int)(q << 14) >> 23; qb[k] = (int)(q << 23) >> 23;
    }
    int n = chain_texels(&s->info);
    int alpha = s->info.format == FMT_AYIQ_8422;
    const BYTE *src = s->data;
    for (int p = 0; p < n; p++) {
        int a = 255, c;
        if (alpha) { c = src[2 * p]; a = src[2 * p + 1]; } else c = src[p];
        int Y = y[c >> 4], I = (c >> 2) & 3, Q = c & 3;
        int r = clamp255(Y + ir[I] + qr[Q]), g = clamp255(Y + ig[I] + qg[Q]), b = clamp255(Y + ib[I] + qb[Q]);
        out[p] = alpha ? (WORD)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4))
                       : (WORD)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

/* ---------------------------------------------------------------- Glide hooks */
static void send_table(int tmu, int which)
{
    Ncc *t = &g_ncc[tmu][which];
    if (!t->sent) { real_DownloadTable(tmu, which, t->raw); t->sent = 1; }
}

static void __stdcall c_DownloadTable(int tmu, int type, void *data)
{
    if ((type == TABLE_NCC0 || type == TABLE_NCC1) && (tmu == 0 || tmu == 1) && data) {
        Ncc *t = &g_ncc[tmu][type];
        memcpy(t->raw, data, NCC_SIZE);
        t->crc = crc_bytes(t->raw, NCC_SIZE);
        t->sent = 0;                  /* nGlide only gets it when a texture is left to it */
        static int logged;
        if (!logged++) {
            const short *iq = (const short *)(t->raw + 16);
            const DWORD *pk = (const DWORD *)(t->raw + 64);
            rr_log("ncc: first table: y0 %u i0 %d,%d,%d packed y %08lx i %08lx", t->raw[0], iq[0], iq[1], iq[2], pk[0], pk[4]);
        }
        return;
    }
    real_DownloadTable(tmu, type, data);
}

static void __stdcall c_NCCTable(int tmu, int table)
{
    if (tmu == 0 || tmu == 1) g_ncc_active[tmu] = table & 1;
    real_NCCTable(tmu, table);
}

static void __stdcall c_DownloadMipMap(int tmu, DWORD addr, DWORD evenOdd, GrTexInfo *info)
{
    real_DownloadMipMap(tmu, addr, evenOdd, info);
    if (tmu < 0 || tmu > 1 || !info) return;
    DWORD end = addr + real_MemRequired(evenOdd, info);
    /* anything this download overwrote is gone */
    for (int i = 0; i < g_nsrc; i++) {
        Src *s = &g_src[i];
        if (s->tmu == tmu && s->addr < end && addr < s->end) {
            free(s->data);
            *s = g_src[--g_nsrc];
            i--;
        }
    }
    if (info->format != FMT_YIQ_422 && info->format != FMT_AYIQ_8422) return;
    if (!info->data || g_nsrc == MAX_SRC) return;
    int bytes = chain_texels(info) * (info->format == FMT_AYIQ_8422 ? 2 : 1);
    BYTE *copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, info->data, bytes);
    Src *s = &g_src[g_nsrc++];
    s->tmu = tmu; s->addr = addr; s->end = end; s->evenOdd = evenOdd;
    s->info = *info; s->info.data = NULL;
    s->data = copy;
    s->gen = ++g_gen;
}

static void __stdcall c_TexSource(int tmu, DWORD addr, DWORD evenOdd, GrTexInfo *info)
{
    if ((tmu == 0 || tmu == 1) && info && (info->format == FMT_YIQ_422 || info->format == FMT_AYIQ_8422)) {
        Src *s = find_src(tmu, addr, evenOdd);
        Ncc *t = &g_ncc[tmu][g_ncc_active[tmu]];
        if (s && s->info.largeLod == info->largeLod && s->info.smallLod == info->smallLod &&
            s->info.aspect == info->aspect && s->info.format == info->format) {
            Entry *e = lookup(tmu, addr, evenOdd, s->gen, t->crc);
            if (!e) { flush(tmu); e = lookup(tmu, addr, evenOdd, s->gen, t->crc); }
            if (e && !e->used) {
                GrTexInfo ci = s->info;
                ci.format = s->info.format == FMT_AYIQ_8422 ? FMT_ARGB_4444 : FMT_RGB_565;
                DWORD size = real_MemRequired(evenOdd, &ci);
                DWORD dst = alloc_area(tmu, size);
                if (!dst) { flush(tmu); dst = alloc_area(tmu, size); e = lookup(tmu, addr, evenOdd, s->gen, t->crc); }
                if (dst && e) {
                    WORD *buf = malloc(chain_texels(&ci) * 2);
                    if (buf) {
                        decode(s, t, buf);
                        ci.data = buf;
                        real_DownloadMipMap(tmu, dst, evenOdd, &ci);
                        free(buf);
                        ci.data = NULL;
                        e->used = 1; e->tmu = tmu; e->addr = addr; e->evenOdd = evenOdd;
                        e->gen = s->gen; e->crc = t->crc; e->dst = dst; e->info = ci;
                        g_decodes++;
                    }
                }
            }
            if (e && e->used) {
                GrTexInfo ci = e->info;
                ci.data = info->data;
                real_TexSource(tmu, e->dst, evenOdd, &ci);
                return;
            }
        }
        send_table(tmu, g_ncc_active[tmu]);   /* not ours: nGlide decodes it as before */
    }
    real_TexSource(tmu, addr, evenOdd, info);
}

static DWORD __stdcall c_MaxAddress(int tmu)
{
    DWORD hi = real_MaxAddress(tmu);
    if (tmu < 0 || tmu > 1) return hi;
    DWORD lo = real_MinAddress(tmu);
    DWORD mid = lo + ((hi - lo) / 2 & ~0xFFFu);
    if (!g_area_hi[tmu]) {
        g_area_lo[tmu] = g_area_next[tmu] = mid;
        g_area_hi[tmu] = hi;
        rr_log("ncc: TMU %d memory %08lx-%08lx, game gets up to %08lx, decoded textures above", tmu, lo, hi, mid);
    }
    return mid;
}

void texcache_report(void)
{
    static DWORD last;
    if (!g_decodes && !g_flushes) return;
    if (GetTickCount() - last < 10000) return;
    last = GetTickCount();
    rr_log("ncc: %d textures decoded, %d cache flushes, %d game textures tracked", g_decodes, g_flushes, g_nsrc);
}

void texcache_install(void)
{
    if (!GetPrivateProfileIntA("window", "glide_ncc_cache", 1, g_ini_path)) return;
    HMODULE exe = GetModuleHandleA(NULL);
    HMODULE gl = GetModuleHandleA("glide2x.dll");
    if (!gl) return;
    real_MinAddress = (void *)GetProcAddress(gl, "_grTexMinAddress@4");
    real_MemRequired = (void *)GetProcAddress(gl, "_grTexTextureMemRequired@8");
    real_DownloadMipMap = (void *)GetProcAddress(gl, "_grTexDownloadMipMap@16");
    real_DownloadTable = (void *)GetProcAddress(gl, "_grTexDownloadTable@12");
    real_NCCTable = (void *)GetProcAddress(gl, "_grTexNCCTable@8");
    real_TexSource = (void *)GetProcAddress(gl, "_grTexSource@16");
    real_MaxAddress = (void *)GetProcAddress(gl, "_grTexMaxAddress@4");
    if (!real_MinAddress || !real_MemRequired || !real_DownloadMipMap || !real_DownloadTable || !real_NCCTable ||
        !real_TexSource || !real_MaxAddress) { rr_log("ncc: Glide functions missing, cache off"); return; }
    rr_hook_iat(exe, "glide2x.dll", "_grTexDownloadMipMap@16", (void *)c_DownloadMipMap);
    rr_hook_iat(exe, "glide2x.dll", "_grTexDownloadTable@12", (void *)c_DownloadTable);
    rr_hook_iat(exe, "glide2x.dll", "_grTexNCCTable@8", (void *)c_NCCTable);
    rr_hook_iat(exe, "glide2x.dll", "_grTexSource@16", (void *)c_TexSource);
    rr_hook_iat(exe, "glide2x.dll", "_grTexMaxAddress@4", (void *)c_MaxAddress);
    rr_log("ncc: YIQ textures decoded once by the patch");
}
