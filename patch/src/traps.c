/* Archipelago traps: received trap items hit a random robot of the player during a mission.

   Oil Trap         an oil slick under the robot (the Oiler's slick, laid by an enemy player: it slows the player's
                    robots)
   Freeze Trap      the robot is frozen like a Nitrogrenade hit (robot +0x3A0 = freeze_timeout)
   Disruptor Trap   the robot malfunctions like a Disrupter hit (robot +0x2C = initial_malfunction_timeout)
   Mine Trap        a mine is stuck to the robot: it is destroyed after 30 seconds
   Nuke Trap        the robot goes off like a Nuke robot: the player's own nuke, so it is destroyed and the blast
                    hits enemy robots and every base in range (the player's too), but spares the player's other robots

   How many of each trap were already set off is kept per seed in rrpatch_ap.ini, so reconnecting or loading a save
   doesn't set them off again; traps received outside a mission wait for the next one.
   The game code and values are found by byte patterns in all three exes (see trap_install). */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define TRAP_FIRST 5200
#define TRAPS 5
static const char *const k_trap_names[TRAPS] = { "Oil Trap", "Freeze Trap", "Disruptor Trap", "Mine Trap", "Nuke Trap" };

/* game */
static void *(__cdecl *g_alloc)(int size, const char *file, int line);
static void (__cdecl *g_register)(void *object);
static void (__thiscall *g_oil_ctor)(void *self, int owner, float *pos, int flag);
static const char *g_oil_file;
static void (__cdecl *g_nuke)(int owner, float *pos, int flag);
static int *g_freeze_timeout, *g_malfunction_timeout;
static int *g_local_player;
static BYTE **g_units;
static int g_ready;

/* robot layout */
#define R_NEXT        0x08
#define R_MALFUNCTION 0x2C
#define R_Z           0x384
#define R_FROZEN      0x3A0
#define R_OWNER       0x414
#define R_X           0x44C
#define R_Y           0x450
#define R_DEAD        0x36E   /* byte: set when destroyed (the robot stays in the list a while) */

/* mines stuck to robots: robot and game time it blows */
static struct { BYTE *robot; DWORD at; } g_mines[16];

static int robot_alive(BYTE *r)
{
    for (BYTE *u = *g_units; u; u = *(BYTE **)(u + R_NEXT)) if (u == r) return !u[R_DEAD];
    return 0;
}

/* robots seen in the list, with the time first seen: a robot just out of the factory isn't a target yet
   (frozen in the factory's exit it was destroyed at once) */
static struct { BYTE *robot; DWORD since; } g_seen[256];
static int g_nseen;

static DWORD seen_since(BYTE *r, DWORD now)
{
    for (int i = 0; i < g_nseen; i++) if (g_seen[i].robot == r) return g_seen[i].since;
    if (g_nseen < 256) { g_seen[g_nseen].robot = r; g_seen[g_nseen].since = now; g_nseen++; }
    return now;
}

static BYTE *random_own_robot(void)
{
    BYTE *list[256];
    int n = 0;
    DWORD now = GetTickCount();
    for (BYTE *u = *g_units; u && n < 256; u = *(BYTE **)(u + R_NEXT))
        if (*(int *)(u + R_OWNER) == *g_local_player && !u[R_DEAD] && now - seen_since(u, now) >= 3000)
            list[n++] = u;
    return n ? list[rand() % n] : NULL;
}

/* an enemy player: the owner of some robot that isn't the player's, else the next player */
static int enemy_player(void)
{
    for (BYTE *u = *g_units; u; u = *(BYTE **)(u + R_NEXT)) {
        int o = *(int *)(u + R_OWNER);
        if (o != *g_local_player && o >= 0 && o < 4) return o;
    }
    return (*g_local_player + 1) & 3;
}

static void robot_pos(BYTE *r, float pos[4])
{
    pos[0] = *(float *)(r + R_X);
    pos[1] = *(float *)(r + R_Y);
    pos[2] = *(float *)(r + R_Z);
    pos[3] = 0;
}

static void destroy(BYTE *r)
{
    void **vtbl = *(void ***)r;
    void (__thiscall *damage)(void *, int, int) = (void (__thiscall *)(void *, int, int))vtbl[0x1C / 4];
    damage(r, 0, 1000000);
}

/* sets off one trap; 0 if there is no robot to hit (it waits) */
static int fire(int trap)
{
    BYTE *r = random_own_robot();
    if (!r) return 0;
    float pos[4];
    robot_pos(r, pos);
    switch (trap) {
    case 0: {
        void *oil = g_alloc(0x4D4, g_oil_file, 0x152);
        if (!oil) return 0;
        g_oil_ctor(oil, enemy_player(), pos, 0);
        g_register(oil);
        break;
    }
    case 1: *(int *)(r + R_FROZEN) = *g_freeze_timeout; break;
    case 2: *(int *)(r + R_MALFUNCTION) = *g_malfunction_timeout; break;
    case 3:
        for (int i = 0; i < 16; i++)
            if (!g_mines[i].robot) { g_mines[i].robot = r; g_mines[i].at = GetTickCount() + 30000; break; }
        break;
    case 4: g_nuke(*g_local_player, pos, 0); destroy(r); break;
    }
    rr_log("trap: %s at %.0f,%.0f", k_trap_names[trap], pos[0], pos[1]);
    return 1;
}

/* from ap.c: the trap items received, how many were set off (per seed), and a message line */
int ap_item_count(int id);
const char *ap_seed(void);
void ap_message(const char *text);

static char *state_path(char *buf, size_t n)
{
    snprintf(buf, n, "%srrpatch_ap.ini", g_game_dir);
    return buf;
}

/* game thread, during missions */
static volatile LONG g_dev_pending[TRAPS];

/* the client's /dev trap (any thread): set off next time a mission has a robot to hit */
void traps_dev_fire(int trap)
{
    if (trap >= 0 && trap < TRAPS) InterlockedIncrement(&g_dev_pending[trap]);
}

void traps_tick(void)
{
    if (!g_ready) return;
    if (g_gui_mode == RR_MODE_SHOP) return;   /* the robot builder is open: the mission waits */
    if (g_gui_mode != RR_MODE_GAMEPLAY) {   /* a mission ended: its robots are gone */
        memset(g_mines, 0, sizeof g_mines);
        g_nseen = 0;
        return;
    }
    DWORD now = GetTickCount();
    {   /* the robots now in the list keep their first-seen time, the others are dropped */
        static struct { BYTE *robot; DWORD since; } keep[256];
        int n = 0;
        for (BYTE *u = *g_units; u && n < 256; u = *(BYTE **)(u + R_NEXT)) {
            keep[n].robot = u;
            keep[n].since = seen_since(u, now);
            n++;
        }
        memcpy(g_seen, keep, n * sizeof keep[0]);
        g_nseen = n;
    }
    for (int i = 0; i < 16; i++) {
        if (!g_mines[i].robot || (int)(now - g_mines[i].at) < 0) continue;
        if (robot_alive(g_mines[i].robot)) {
            rr_log("trap: the mine went off");
            destroy(g_mines[i].robot);
        }
        g_mines[i].robot = NULL;
    }
    /* traps asked for by the client's /dev trap: wait until there is a robot to hit */
    for (int t = 0; t < TRAPS; t++)
        if (g_dev_pending[t] > 0 && fire(t)) { InterlockedDecrement(&g_dev_pending[t]); rr_log("trap: dev %s", k_trap_names[t]); }
    /* [ap] trap_keys=1 in rrpatch.ini (testing): F5-F9 set off one trap each */
    static int keys = -1;
    if (keys < 0) keys = GetPrivateProfileIntA("ap", "trap_keys", 0, g_ini_path);
    if (keys && GetForegroundWindow() == rr_game_hwnd()) {
        static int down[TRAPS];
        for (int t = 0; t < TRAPS; t++) {
            int d = (GetAsyncKeyState(VK_F5 + t) & 0x8000) != 0;
            if (d && !down[t]) { rr_log("trap: key F%d", 5 + t); fire(t); }
            down[t] = d;
        }
    }
    static DWORD last;
    if (now - last < 1000) return;   /* at most one trap a second */
    last = now;
    const char *seed = ap_seed();
    if (!seed || !seed[0]) return;
    char path[MAX_PATH], key[32], val[16];
    state_path(path, sizeof path);
    for (int t = 0; t < TRAPS; t++) {
        snprintf(key, sizeof key, "trap_%d_applied", t);
        int done = GetPrivateProfileIntA(seed, key, 0, path);
        if (ap_item_count(TRAP_FIRST + t) <= done) continue;
        if (!fire(t)) return;
        snprintf(val, sizeof val, "%d", done + 1);
        WritePrivateProfileStringA(seed, key, val, path);
        char msg[64];
        snprintf(msg, sizeof msg, "%s!", k_trap_names[t]);
        ap_message(msg);
        return;   /* one per second */
    }
}

/* ---------------------------------------------------------------- finding the game code */
static BYTE *rel(BYTE *at) { return at + 4 + *(int *)at; }

static BYTE *scan(const BYTE *pat, const char *mask)
{
    size_t len = strlen(mask);
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - len;
        for (; p <= end; p++) {
            size_t i = 0;
            while (i < len && (mask[i] == '?' || p[i] == pat[i])) i++;
            if (i == len) return p;
        }
    }
    return NULL;
}

void traps_install(int *local_player)
{
    const RRGame *G = rr_game();
    if (!G || !G->units || !local_player) { rr_log("trap: robot list not found, no traps"); return; }
    g_units = G->units;
    g_local_player = local_player;

    /* the Nuke head firing: ...; mov [esp+4],eax; call nuke_spawn */
    static const BYTE nuke[] = { 0x8B, 0x82, 0x84, 0x03, 0, 0, 0x89, 0x44, 0x24, 0x14, 0x8B, 0x82, 0x14, 0x04, 0, 0,
                                 0x8A, 0x8A, 0x8A, 0x04, 0, 0, 0x89, 0x04, 0x24, 0x8D, 0x44, 0x24, 0x0C, 0x89, 0x4C,
                                 0x24, 0x08, 0x89, 0x44, 0x24, 0x04, 0xE8 };
    BYTE *p = scan(nuke, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    if (p) g_nuke = (void *)rel(p + 38);
    /* the nitro hit: mov eax,[freeze_timeout]; mov edx,40h; mov [ebx+3A0h],eax */
    static const BYTE frz[] = { 0xA1, 0, 0, 0, 0, 0xBA, 0x40, 0, 0, 0, 0x89, 0x83, 0xA0, 0x03, 0, 0 };
    p = scan(frz, "x????xxxxxxxxxxx");
    if (p) g_freeze_timeout = *(int **)(p + 1);
    /* the Disrupter hit: mov eax,[malfunction_timeout]; mov [edx+2Ch],eax; jmp */
    static const BYTE mlf[] = { 0xA1, 0, 0, 0, 0, 0x89, 0x42, 0x2C, 0xEB };
    p = scan(mlf, "x????xxxx");
    if (p) g_malfunction_timeout = *(int **)(p + 1);
    /* an oil grenade landing: mov eax,4D4h; mov edx,"oil.cpp"; ...; call alloc */
    static const BYTE oil[] = { 0xB8, 0xD4, 0x04, 0, 0, 0xBA, 0, 0, 0, 0, 0xB9, 0, 0, 0, 0, 0x89, 0x04, 0x24, 0x89, 0x4C,
                                0x24, 0x08, 0x89, 0x54, 0x24, 0x04, 0xE8 };
    p = scan(oil, "xxxxxx????x??xxxxxxxxxxxxxx");
    if (p) { g_oil_file = *(const char **)(p + 6); g_alloc = (void *)rel(p + 27); }
    /* ... mov ecx,ebp; call oil_ctor; mov eax,ebp; mov [esp],eax; call register; mov dword [ebx+4],1 */
    static const BYTE ctor[] = { 0x8B, 0xCD, 0xE8, 0, 0, 0, 0, 0x8B, 0xC5, 0x89, 0x04, 0x24, 0xE8, 0, 0, 0, 0,
                                 0xC7, 0x43, 0x04, 0x01, 0, 0, 0 };
    p = scan(ctor, "xxx????xxxxxx????xxxxxxx");
    if (p) { g_oil_ctor = (void *)rel(p + 3); g_register = (void *)rel(p + 13); }

    g_ready = g_nuke && g_freeze_timeout && g_malfunction_timeout && g_oil_file && g_alloc && g_oil_ctor && g_register;
    rr_log("trap: %s (nuke %p, freeze %p, malfunction %p, oil %p/%p/%p)", g_ready ? "ready" : "code not found, no traps",
           (void *)g_nuke, (void *)g_freeze_timeout, (void *)g_malfunction_timeout, (void *)g_alloc,
           (void *)g_oil_ctor, (void *)g_register);
}
