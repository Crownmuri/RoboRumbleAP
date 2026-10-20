/* Dev mode cheats, switched by the client's /dev commands (host.yaml dev_mode); see ap.c for the DEV lines.

   god        the player's robots take no damage and can't be frozen: robot +0x484 = 2, the alien Defense
              Booster's field (the damage functions deal 0 at 2), +0x3A0 (frozen) = 0. The player's base keeps the
              highest energy and level count it had since god mode went on.
   nolose     a lost mission isn't lost: the frame code's "win flag == 2" branch is hooked and clears the flag
              (the hidden rules like "3 robos lost" and every other loss go through that flag)
   funds      in-mission Robo Funds stay at 99999 (player struct +0x0C)
   money      the Money for the shop stays at 99 000 000 (campaign state +8)
   alien      an alien booster pickup (map tile bits 0x380: 1 Speed, 2 Defense, 3 Weapon, 4 Intelligence) next to
              one of the player's robots, or the booster given to all of them at once

   Located without per-exe addresses:
     players  4 player pointers, the local player's index right after them (the pointer traps.c gets)
     bases    0x270 after the players: 4 x 0x8C bytes, +0 alive, +0x24 energy, +0x28 energy levels left
     tiles    the robot's tile lookup: mov edx,[edx*8+tiles]; test edx,380h (tile = (y*256 + x*2)*8 bytes,
              200 world units per tile) */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define R_NEXT    0x08
#define R_DEAD    0x36E
#define R_FROZEN  0x3A0
#define R_OWNER   0x414
#define R_X       0x44C
#define R_Y       0x450
#define R_DEFENSE 0x484
#define R_SPEED   0x488
#define R_WEAPON  0x48A
#define R_INTEL   0x48B
#define TILE_SIZE 200

static const RRGame *G;
static int *g_local;          /* local player index */
static BYTE **g_players;      /* 4 player structs */
static BYTE *g_bases;
static DWORD *g_tiles;
static int g_ready;

static volatile LONG g_god, g_nolose, g_funds, g_money;
static volatile LONG g_alien_pending[5], g_alien_all[5];

/* ---------------------------------------------------------------- no loss */
DWORD *g_dev_winflag;
void *g_lose_target, *g_nolose_resume;
volatile LONG *g_dev_nolose_flag = &g_nolose;
__asm__(
    ".text\n"
    ".globl _nolose_stub\n"
    "_nolose_stub:\n"
    "    cmpl $2, %eax\n"
    "    jne 1f\n"
    "    pushl %edx\n"
    "    movl _g_dev_nolose_flag, %edx\n"
    "    cmpl $0, (%edx)\n"
    "    popl %edx\n"
    "    jne 2f\n"
    "    jmp *_g_lose_target\n"
    "2:  movl _g_dev_winflag, %eax\n"
    "    movl $0, (%eax)\n"
    "    xorl %eax, %eax\n"
    "1:  jmp *_g_nolose_resume\n");
void nolose_stub(void);

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

void dev_install(int *local_player)
{
    G = rr_game();
    if (!G || !G->units || !local_player) { rr_log("dev: game data not found, no dev cheats"); return; }
    g_local = local_player;
    g_players = (BYTE **)(local_player - 4);
    g_bases = (BYTE *)local_player - 0x10 + 0x270;

    static const BYTE tiles[] = { 0x8B, 0x14, 0xD5, 0, 0, 0, 0, 0xF7, 0xC2, 0x80, 0x03, 0, 0 };
    BYTE *p = scan(tiles, "xxx????xxxxxx");
    if (p) g_tiles = *(DWORD **)(p + 3);

    /* mov eax,[win]; cmp eax,1; je win; cmp eax,2; je lose  (the frame code acting on the win flag) */
    if (G->winflag) {
        BYTE pat[23] = { 0xA1, 0, 0, 0, 0, 0x83, 0xF8, 0x01, 0x0F, 0x84, 0, 0, 0, 0, 0x83, 0xF8, 0x02, 0x0F, 0x84 };
        memcpy(pat + 1, &G->winflag, 4);
        p = scan(pat, "xxxxxxxxxx????xxxxx????");
        if (p) {
            g_dev_winflag = G->winflag;
            g_lose_target = p + 23 + *(int *)(p + 19);
            g_nolose_resume = p + 23;
            if (!rr_write_jmp(p + 14, (void *)nolose_stub, 9)) p = NULL;
        }
        if (!p) rr_log("dev: loss branch not found, no nolose");
    }
    g_ready = 1;
    rr_log("dev: cheats ready (players %p, bases %p, tiles %p, lose %p)", (void *)g_players, (void *)g_bases,
           (void *)g_tiles, g_lose_target);
}

/* ---------------------------------------------------------------- requests (any thread) */
void dev_set(const char *what, int on)
{
    if (!strcmp(what, "GOD")) InterlockedExchange(&g_god, on);
    else if (!strcmp(what, "NOLOSE")) InterlockedExchange(&g_nolose, on);
    else if (!strcmp(what, "FUNDS")) InterlockedExchange(&g_funds, on);
    else if (!strcmp(what, "INFMONEY")) InterlockedExchange(&g_money, on);
    rr_log("dev: %s %s", what, on ? "on" : "off");
}

/* type 1 Speed, 2 Defense, 3 Weapon, 4 Intelligence; all = give it to every robot instead of dropping one */
void dev_alien(int type, int all)
{
    if (type < 1 || type > 4) return;
    InterlockedIncrement(all ? &g_alien_all[type] : &g_alien_pending[type]);
}

/* ---------------------------------------------------------------- game thread */
static int own(BYTE *r) { return *(int *)(r + R_OWNER) == *g_local && !r[R_DEAD]; }

/* robots god mode changed: their own +0x484 comes back when it goes off */
static struct { BYTE *robot; int defense; } g_saved[256];
static int g_nsaved;

static int in_list(BYTE *r)
{
    for (BYTE *u = *G->units; u; u = *(BYTE **)(u + R_NEXT)) if (u == r) return 1;
    return 0;
}

static void god_off(void)
{
    for (int i = 0; i < g_nsaved; i++)
        if (in_list(g_saved[i].robot) && *(int *)(g_saved[i].robot + R_DEFENSE) == 2)
            *(int *)(g_saved[i].robot + R_DEFENSE) = g_saved[i].defense;
    g_nsaved = 0;
}

static void god_tick(void)
{
    for (BYTE *u = *G->units; u; u = *(BYTE **)(u + R_NEXT)) {
        if (!own(u)) continue;
        int known = 0;
        for (int i = 0; i < g_nsaved && !known; i++) known = g_saved[i].robot == u;
        if (!known && g_nsaved < 256) { g_saved[g_nsaved].robot = u; g_saved[g_nsaved].defense = *(int *)(u + R_DEFENSE); g_nsaved++; }
        *(int *)(u + R_DEFENSE) = 2;
        *(int *)(u + R_FROZEN) = 0;
    }
    static int energy, levels;
    BYTE *b = g_bases + 0x8C * *g_local;
    if (*g_local < 0 || *g_local > 3 || !b[0]) return;
    int *e = (int *)(b + 0x24), *l = (int *)(b + 0x28);
    static BYTE *last_base;
    if (b != last_base || *e > energy) energy = *e;
    if (b != last_base || *l > levels) levels = *l;
    last_base = b;
    *e = energy;
    *l = levels;
}

static BYTE *random_own_robot(void)
{
    BYTE *list[256];
    int n = 0, sel = 0;
    for (BYTE *u = *G->units; u && n < 256; u = *(BYTE **)(u + R_NEXT)) {
        if (!own(u)) continue;
        if (u[0x39D] && !sel) { sel = 1; n = 0; }   /* a selected robot wins over the others */
        if (sel && !u[0x39D]) continue;
        list[n++] = u;
    }
    return n ? list[rand() % n] : NULL;
}

static const char *const k_alien[] = { "", "Speed", "Defense", "Weapon", "Intelligence" };

static int drop_alien(int type)
{
    BYTE *r = random_own_robot();
    if (!r || !g_tiles) return 0;
    int tx = (int)(*(float *)(r + R_X) / TILE_SIZE), ty = (int)(*(float *)(r + R_Y) / TILE_SIZE);
    /* the first free tile around the robot (its own tile would only be picked up once it moves off and back) */
    static const int d[8][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    for (int i = 0; i < 8; i++) {
        int x = tx + d[i][0], y = ty + d[i][1];
        if (x < 0 || y < 0 || x > 127 || y > 127) continue;
        DWORD *t = g_tiles + (y * 256 + x * 2) * 2;
        if (*t & 0x380) continue;
        *t = (*t & ~0x380u) | ((DWORD)type << 7);
        rr_log("dev: alien %s booster dropped at tile %d,%d", k_alien[type], x, y);
        return 1;
    }
    return 0;
}

static void give_alien(int type)
{
    int n = 0;
    for (BYTE *u = *G->units; u; u = *(BYTE **)(u + R_NEXT)) {
        if (!own(u)) continue;
        switch (type) {
        case 1: u[R_SPEED] = 1; break;
        case 2: if (*(int *)(u + R_DEFENSE) == 0) *(int *)(u + R_DEFENSE) = 300; break;
        case 3: u[R_WEAPON] = 1; break;
        case 4: u[R_INTEL] = 1; break;
        }
        n++;
    }
    rr_log("dev: alien %s booster given to %d robots", k_alien[type], n);
}

/* Progressive Starting Robo Funds (ap.c, at the start of a mission) */
void rr_add_robo_funds(int amount)
{
    if (!g_ready || *g_local < 0 || *g_local > 3 || !g_players[*g_local]) return;
    int *funds = (int *)(g_players[*g_local] + 0x0C);
    rr_log("ap: Robo Funds %d + %d", *funds, amount);
    *funds += amount;
}

void dev_tick(void)
{
    if (!g_ready) return;
    if (g_money) *(int *)(G->state + 8) = 99000000;
    if (g_gui_mode != RR_MODE_GAMEPLAY) {
        g_nsaved = 0;   /* the mission's robots are gone */
        return;
    }
    if (g_god) god_tick();
    else if (g_nsaved) god_off();
    if (g_funds && *g_local >= 0 && *g_local < 4 && g_players[*g_local])
        *(int *)(g_players[*g_local] + 0x0C) = 99999;
    for (int t = 1; t <= 4; t++) {
        if (g_alien_all[t] > 0) { InterlockedDecrement(&g_alien_all[t]); give_alien(t); }
        if (g_alien_pending[t] > 0 && drop_alien(t)) InterlockedDecrement(&g_alien_pending[t]);
    }
}
