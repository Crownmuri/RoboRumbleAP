/* File overrides.

   The game opens data files through its own VFS (rr_soft 0x4EF634), which looks in the
   loaded .pak archives first and only then on disk, so loose files never win. This hooks
   the start of that function and swaps the file name for "<override_folder>\<name>" when
   such a file exists. The new name is not inside any pak, so the VFS falls through to disk.

   adjust.ini is special: rrpatch.ini's [adjust] section is merged into it at startup
   (written to rrpatch_adjust.ini), so tweaks like the zoom range need no copy of the file. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* push edi; push esi; push ebp; push ebx; sub esp,54h; mov [esp+4Ch],ecx; lea ebp,[esp+8];
   call +10Ch; mov eax,[esp+4Ch]  -- identical in rr_soft, rr_dx5 and rr_glide */
static const BYTE k_vfs_open_sig[] = {
    0x57, 0x56, 0x55, 0x53, 0x83, 0xEC, 0x54, 0x89, 0x4C, 0x24, 0x4C, 0x8D,
    0x6C, 0x24, 0x08, 0xE8, 0x0C, 0x01, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x4C,
};
#define VFS_STOLEN 7          /* the four pushes + sub esp,54h */
#define VFS_NAME_MAX 59       /* the game copies the name into a 60-byte stack buffer */

static const char *k_protected_keys[] = {
    /* basic_level_size=254 is what switches the CD copy check off; the *_lock keys look
       like more of the same. Never let a merge change them. */
    "basic_level_size", "tw_lock", "fair_lock", "demo_lock",
};

static char g_merged_adjust[MAX_PATH];  /* relative name handed to the game, "" if none */
void *g_vfs_resume;                     /* address after the stolen bytes */

/* files generated at startup (e.g. quieter sound effects): game name -> relative path */
#define MAX_GENERATED 128
static struct { char name[64]; char path[VFS_NAME_MAX + 1]; } g_generated[MAX_GENERATED];
static int g_generated_count;
static int g_log_files;            /* [general] log_files=1: log every file the game reads */

static int has_write_mode(const char *mode)
{
    for (; mode && *mode; mode++)
        if (*mode == 'w' || *mode == 'a' || *mode == '+') return 1;
    return 0;
}

/* Called from the stub with the game's arguments. Returns a replacement name or NULL. */
const char *__cdecl vfs_redirect(const char *name, const char *mode)
{
    static char buf[4][VFS_NAME_MAX + 1];
    static int slot;
    if (!name || has_write_mode(mode)) return NULL;
    if (g_log_files) rr_log("file: %s", name);
    if (g_merged_adjust[0] && lstrcmpiA(name, "adjust.ini") == 0) return g_merged_adjust;
    for (int i = 0; i < g_generated_count; i++)
        if (lstrcmpiA(name, g_generated[i].name) == 0) return g_generated[i].path;
    if (!g_cfg.override_enabled) return NULL;
    char *out = buf[slot++ & 3];
    if (snprintf(out, VFS_NAME_MAX + 1, "%s\\%s", g_cfg.override_folder, name) > VFS_NAME_MAX) return NULL;
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return NULL;
    rr_log("vfs: %s -> %s", name, out);
    return out;
}

/* jumped to from the patched function start; the game's return address is at [esp] */
__asm__(
    ".text\n"
    ".globl _vfs_stub\n"
    "_vfs_stub:\n"
    "    pushal\n"                  /* 32 bytes: name at 36(%esp), mode at 40(%esp) */
    "    pushl 40(%esp)\n"          /* mode */
    "    pushl 40(%esp)\n"          /* name */
    "    call _vfs_redirect\n"
    "    addl $8, %esp\n"
    "    testl %eax, %eax\n"
    "    jz 1f\n"
    "    movl %eax, 36(%esp)\n"
    "1:  popal\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    subl $0x54, %esp\n"
    "    jmp *_g_vfs_resume\n");
void vfs_stub(void);

/* ---- reading a file out of a VFSD archive ---- */
static char *pak_read(const char *pak, const char *entry, DWORD *size)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s", g_game_dir, pak);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *result = NULL;
    char magic[4];
    DWORD count;
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "VFSD", 4) == 0 && fread(&count, 4, 1, f) == 1) {
        for (DWORD i = 0; i < count; i++) {
            int len = fgetc(f);
            char name[256];
            DWORD sz;
            if (len < 0 || fread(name, 1, len, f) != (size_t)len || fread(&sz, 4, 1, f) != 1) break;
            name[len] = 0;
            if (lstrcmpiA(name, entry) == 0) {
                result = malloc(sz + 1);
                if (result && fread(result, 1, sz, f) == sz) { result[sz] = 0; *size = sz; }
                else { free(result); result = NULL; }
                break;
            }
            fseek(f, sz, SEEK_CUR);
        }
    }
    fclose(f);
    return result;
}

static char *file_read(const char *path, DWORD *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc(n + 1) : NULL;
    if (buf && fread(buf, 1, n, f) == (size_t)n) { buf[n] = 0; *size = (DWORD)n; }
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static int is_protected(const char *key)
{
    for (size_t i = 0; i < sizeof k_protected_keys / sizeof *k_protected_keys; i++)
        if (lstrcmpiA(key, k_protected_keys[i]) == 0) return 1;
    return 0;
}

/* key of an ini line ("  max_zoom = 1.0" -> "max_zoom") into out; returns 0 if none */
static int line_key(const char *line, size_t len, char *out, size_t outsz)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t start = i;
    while (i < len && line[i] != '=') i++;
    if (i >= len || line[start] == '[' || line[start] == ';') return 0;
    size_t end = i;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    if (end == start || end - start >= outsz) return 0;
    memcpy(out, line + start, end - start);
    out[end - start] = 0;
    return 1;
}

/* Merges [adjust] from rrpatch.ini into adjust.ini and writes rrpatch_adjust.ini. */
static void build_merged_adjust(void)
{
    static char section[32768];
    DWORD n = GetPrivateProfileSectionA("adjust", section, sizeof section, g_ini_path);
    if (n == 0) return;

    /* the overrides: "key=value\0key=value\0\0" */
    enum { MAXO = 256 };
    char *okey[MAXO], *oval[MAXO];
    int oused[MAXO], count = 0;
    for (char *p = section; *p && count < MAXO; p += strlen(p) + 1) {
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!*k) continue;
        if (is_protected(k)) { rr_log("adjust: ignoring protected key %s", k); continue; }
        okey[count] = k; oval[count] = v; oused[count] = 0; count++;
    }
    if (!count) return;

    DWORD size = 0;
    char override_path[MAX_PATH];
    snprintf(override_path, sizeof override_path, "%s%s\\adjust.ini", g_game_dir, g_cfg.override_folder);
    char *src = g_cfg.override_enabled ? file_read(override_path, &size) : NULL;
    if (!src) src = pak_read("pak\\ini.pak", "adjust.ini", &size);
    if (!src) { rr_log("adjust: could not read adjust.ini from pak\\ini.pak"); return; }

    char out_path[MAX_PATH];
    snprintf(out_path, sizeof out_path, "%srrpatch_adjust.ini", g_game_dir);
    FILE *out = fopen(out_path, "wb");
    if (!out) { free(src); rr_log("adjust: cannot write %s", out_path); return; }

    int header_done = 0;
    for (char *line = src; line < src + size;) {
        char *eol = line;
        while (eol < src + size && *eol != '\n') eol++;
        size_t len = eol - line;
        if (len && line[len - 1] == '\r') len--;
        char key[128];
        int replaced = 0;
        if (line_key(line, len, key, sizeof key)) {
            for (int i = 0; i < count; i++) {
                if (lstrcmpiA(key, okey[i]) == 0) {
                    fprintf(out, "%s=%s\r\n", key, oval[i]);
                    oused[i] = 1;
                    replaced = 1;
                    break;
                }
            }
        }
        if (!replaced) { fwrite(line, 1, len, out); fputs("\r\n", out); }
        /* keys the file does not have go right under the section header */
        if (!header_done && len >= 13 && _strnicmp(line, "[Adjustables]", 13) == 0) {
            header_done = 1;
            for (int i = 0; i < count; i++) {
                int present = 0;
                for (char *s = eol; s < src + size && !present;) {
                    char *e = s;
                    while (e < src + size && *e != '\n') e++;
                    char k2[128];
                    size_t l2 = e - s;
                    if (l2 && s[l2 - 1] == '\r') l2--;
                    if (line_key(s, l2, k2, sizeof k2) && lstrcmpiA(k2, okey[i]) == 0) present = 1;
                    s = e + 1;
                }
                if (!present) { fprintf(out, "%s=%s\r\n", okey[i], oval[i]); oused[i] = 1; }
            }
        }
        line = eol + 1;
    }
    fclose(out);
    free(src);
    lstrcpyA(g_merged_adjust, "rrpatch_adjust.ini");
    for (int i = 0; i < count; i++) rr_log("adjust: %s=%s%s", okey[i], oval[i], oused[i] ? "" : " (not written)");
}

/* Some effects were recorded too hot: their peaks sit flat at full scale. Each flat stretch
   is replaced by a cubic Hermite curve through its neighbours (the waveform it was cut from,
   roughly), which may go past full scale; the gain applied afterwards makes room for it.
   Returns the number of stretches rebuilt. */
static int declip(float *s, DWORD n)
{
    const float limit = 32700.0f;
    int runs = 0;
    for (DWORD i = 2; i + 2 < n; i++) {
        if (s[i] < limit && s[i] > -limit) continue;
        DWORD j = i;
        float sign = s[i] > 0 ? 1.0f : -1.0f;
        while (j + 2 < n && s[j] * sign >= limit) j++;
        /* stretch is [i, j); anchors are i-1 and j, tangents from one sample further out */
        float p0 = s[i - 1], p1 = s[j];
        float m0 = s[i - 1] - s[i - 2], m1 = s[j + 1] - s[j];
        float len = (float)(j - i + 1);
        m0 *= len; m1 *= len;
        for (DWORD k = i; k < j; k++) {
            float t = (float)(k - i + 1) / len, t2 = t * t, t3 = t2 * t;
            float v = (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
            if (v * sign < limit) v = limit * sign;   /* never below what the recording kept */
            if (v * sign > 2.0f * limit) v = 2.0f * limit * sign;
            s[k] = v;
        }
        runs++;
        i = j;
    }
    return runs;
}

/* Sound effects. With [sfx] auto_fix=1 every effect in pak\wav.pak whose recording is
   clipped or starts/ends abruptly gets fixed: clipped stretches rebuilt (declip), the level
   lowered just enough for the rebuilt peaks, and 3 ms fades at hard edges (those click).
   [sfx_gain] "wav\door.wav=-3" adds a manual change in dB on top. Fixed copies go to
   rrpatch_gen\ and the game is redirected to them. */
static double sfx_manual_db(const char *name)
{
    char v[32];
    GetPrivateProfileStringA("sfx_gain", name, "0", v, sizeof v, g_ini_path);
    return atof(v);
}

static void fade_edges(float *f, DWORD n, int rate, int *faded)
{
    DWORD len = (DWORD)(rate * 3 / 1000);
    if (n < len * 4) return;
    if (fabsf(f[0]) > 1000.0f) {
        for (DWORD i = 0; i < len; i++) f[i] *= (float)i / len;
        *faded |= 1;
    }
    if (fabsf(f[n - 1]) > 1000.0f) {
        for (DWORD i = 0; i < len; i++) f[n - 1 - i] *= (float)i / len;
        *faded |= 2;
    }
}

/* Fixes one WAV in place. Returns 1 if anything changed. */
static int fix_wav(const char *name, BYTE *wav, DWORD size, int auto_fix)
{
    if (size < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return 0;
    int bits = 0, rate = 22050, changed = 0;
    double manual = sfx_manual_db(name);
    for (DWORD pos = 12; pos + 8 <= size;) {
        DWORD csz = *(DWORD *)(wav + pos + 4);
        if (memcmp(wav + pos, "fmt ", 4) == 0 && csz >= 16) {
            rate = *(DWORD *)(wav + pos + 12);
            bits = *(WORD *)(wav + pos + 8 + 14);
        }
        if (memcmp(wav + pos, "data", 4) == 0 && (bits == 16 || bits == 8)) {
            DWORD bytes = pos + 8 + csz <= size ? csz : size - pos - 8;
            DWORD n = bits == 16 ? bytes / 2 : bytes;
            float *f = malloc(n * sizeof(float));
            if (!f) return 0;
            short *s16 = (short *)(wav + pos + 8);
            BYTE *s8 = wav + pos + 8;
            for (DWORD i = 0; i < n; i++) f[i] = bits == 16 ? s16[i] : (s8[i] - 128) * 256.0f;
            int runs = 0, faded = 0;
            double gain = pow(10.0, manual / 20.0);
            /* [sfx_trim] name=ms: silence the start (e.g. the scrape before the door slam) */
            char trim_s[16];
            GetPrivateProfileStringA("sfx_trim", name, "0", trim_s, sizeof trim_s, g_ini_path);
            DWORD trim = (DWORD)(atof(trim_s) * rate / 1000.0), ramp = (DWORD)(rate / 200);
            if (trim > 0 && trim + ramp < n) {
                for (DWORD i = 0; i < trim; i++) f[i] = 0;
                for (DWORD i = 0; i < ramp; i++) f[trim + i] *= (float)i / ramp;
                faded |= 4;
            }
            if (auto_fix) {
                if (bits == 16) runs = declip(f, n);
                fade_edges(f, n, rate, &faded);
                float peak = 0;
                for (DWORD i = 0; i < n; i++) if (fabsf(f[i]) > peak) peak = fabsf(f[i]);
                if (runs && peak > 31000.0f) gain *= 31000.0 / peak;
            }
            if (runs || faded || gain != 1.0) {
                for (DWORD i = 0; i < n; i++) {
                    double v = f[i] * gain;
                    if (bits == 16) s16[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    else { v = v / 256.0 + 128; s8[i] = (BYTE)(v > 255 ? 255 : v < 0 ? 0 : v); }
                }
                rr_log("sfx: %s%s%s%s%s, %+.1f dB", name, runs ? " declipped" : "", faded & 1 ? " fade-in" : "",
                       faded & 2 ? " fade-out" : "", faded & 4 ? " start silenced" : "", 20.0 * log10(gain));
                changed = 1;
            }
            free(f);
        }
        pos += 8 + csz + (csz & 1);
    }
    return changed;
}

static void build_sfx(void)
{
    int auto_fix = GetPrivateProfileIntA("sfx", "auto_fix", 1, g_ini_path);
    char section[4];
    int any_manual = GetPrivateProfileSectionA("sfx_gain", section, sizeof section, g_ini_path) > 0 ||
                     GetPrivateProfileSectionA("sfx_trim", section, sizeof section, g_ini_path) > 0;
    if (!auto_fix && !any_manual) return;

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%spak\\wav.pak", g_game_dir);
    DWORD size = 0;
    BYTE *pak = (BYTE *)file_read(path, &size);
    if (!pak || size < 8 || memcmp(pak, "VFSD", 4) != 0) { free(pak); rr_log("sfx: cannot read pak\\wav.pak"); return; }
    DWORD count = *(DWORD *)(pak + 4), p = 8;
    for (DWORD i = 0; i < count && p < size && g_generated_count < MAX_GENERATED; i++) {
        int len = pak[p];
        char name[256];
        if (p + 1 + len + 4 > size) break;
        memcpy(name, pak + p + 1, len);
        name[len] = 0;
        DWORD esz = *(DWORD *)(pak + p + 1 + len);
        BYTE *data = pak + p + 1 + len + 4;
        p += 1 + len + 4 + esz;
        if (p > size) break;
        if (!fix_wav(name, data, esz, auto_fix)) continue;

        char rel[VFS_NAME_MAX + 1], full[MAX_PATH];
        if (snprintf(rel, sizeof rel, "rrpatch_gen\\%s", name) > VFS_NAME_MAX) continue;
        snprintf(full, sizeof full, "%s%s", g_game_dir, rel);
        for (char *s = full + strlen(g_game_dir); *s; s++)   /* create the folders */
            if (*s == '\\') { *s = 0; CreateDirectoryA(full, NULL); *s = '\\'; }
        FILE *f = fopen(full, "wb");
        if (!f) continue;
        fwrite(data, 1, esz, f);
        fclose(f);
        lstrcpynA(g_generated[g_generated_count].name, name, sizeof g_generated[0].name);
        lstrcpyA(g_generated[g_generated_count].path, rel);
        g_generated_count++;
    }
    free(pak);
}

/* ---- for other parts of the patch ---- */
char *rr_pak_read(const char *pak, const char *entry, DWORD *size)
{
    return pak_read(pak, entry, size);
}

/* Writes rrpatch_gen\<name> and hands it to the game whenever it opens <name>. */
int rr_vfs_generate(const char *name, const void *data, DWORD size)
{
    if (g_generated_count >= MAX_GENERATED) return 0;
    char rel[VFS_NAME_MAX + 1], full[MAX_PATH];
    if (snprintf(rel, sizeof rel, "rrpatch_gen\\%s", name) > VFS_NAME_MAX) return 0;
    snprintf(full, sizeof full, "%srrpatch_gen", g_game_dir);
    CreateDirectoryA(full, NULL);
    snprintf(full, sizeof full, "%s%s", g_game_dir, rel);
    FILE *f = fopen(full, "wb");
    if (!f) return 0;
    fwrite(data, 1, size, f);
    fclose(f);
    lstrcpynA(g_generated[g_generated_count].name, name, sizeof g_generated[0].name);
    lstrcpyA(g_generated[g_generated_count].path, rel);
    g_generated_count++;
    return 1;
}

void vfs_install(void)
{
    g_log_files = GetPrivateProfileIntA("general", "log_files", 0, g_ini_path);
    build_merged_adjust();
    build_sfx();
    apdlg_generate();
    if (!g_merged_adjust[0] && !g_generated_count && !g_cfg.override_enabled && !g_log_files) return;
    BYTE *fn = rr_find_unique(k_vfs_open_sig, sizeof k_vfs_open_sig);
    if (!fn) { rr_log("vfs: open function not found, overrides off"); g_merged_adjust[0] = 0; return; }
    g_vfs_resume = fn + VFS_STOLEN;
    if (!rr_write_jmp(fn, (void *)vfs_stub, VFS_STOLEN)) { rr_log("vfs: patch failed"); return; }
    rr_log("vfs: hooked open at %p", fn);
}
