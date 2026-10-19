/* Archipelago ([ap] enabled=1).

   The patch connects to the RoboRumble client (apworld/roborumble/client.py) on 127.0.0.1:[ap] port and talks
   in text lines:
     client -> game   SLOT <side> <open missions> <seed>      the slot, sent first
                      ITEMS <id>,<id>,...                      every item received so far, in order (repeats count)
                      CHECKED <id>,<id>,...                    locations the server has as checked
                      MISSION <n>                              play mission n (1-16) next
                      MSG <text>                               a line for the log / later the screen
     game -> client   HELLO <exe>
                      CHECK <id>                               a location was reached
                      STATE <mode> <mission>                   where the game is, for the client's display
   Ids are those of apworld/roborumble/data.py.

   What the patch does with them (all on the game thread, from the message loop):
   - the robot builder's weapon and legs masks show exactly the parts received, for the slot's side
   - boosters, base upgrades and inventions are "owned" while a mission runs if they were received
   - in the briefing the engineer's shop shows what is still a check: unchecked goods can be bought, checked ones
     are greyed; a purchase sends its check instead of giving the good
   - mission won, robot finished (weapon, legs, combination, booster), spy report bought -> checks
   - missions: when the briefing or the level loading comes up, the mission to play is switched to the one the
     player chose (MISSION) or else to the first unlocked mission that isn't won yet */
#include "common.h"
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>

#define HEAD_OFFSET 1000
#define LEGS_OFFSET 2000
#define BOOSTER_OFFSET 3000
#define UPGRADE_OFFSET 3100
#define INVENTION_OFFSET 3200
#define MISSION_OFFSET 4000
#define FILLER_SHOP_MONEY 5002
#define COMBO_OFFSET 6000
#define SHOP_OFFSET 7000
#define SPY_OFFSET 7300

#define SHOP_SLOTS 22   /* 9 boosters, U1-U10, S1-S3 */
#define SHOP_MONEY 500000

static CRITICAL_SECTION g_cs;
static int g_enabled, g_port;

/* received from the client (under g_cs) */
static int g_have_slot, g_side_mode, g_final_wins;   /* side mode 0 Red Star, 1 Terraflux, 2 both */
static char g_seed[64];
static int *g_items, g_nitems;
static int *g_checked, g_nchecked;
static int g_choose_mission, g_choose_side;   /* MISSION side n, mission 0 = none */
static volatile LONG g_dirty;         /* items/checked changed */

/* outgoing lines */
static char g_out[64][96];
static int g_nout;
static volatile LONG g_connected;

static const RRGame *G;
static int *g_spy_price;

/* ---------------------------------------------------------------- helpers */
static int count_item(int id)
{
    int n = 0;
    for (int i = 0; i < g_nitems; i++) n += g_items[i] == id;
    return n;
}

static int is_checked(int id)
{
    for (int i = 0; i < g_nchecked; i++) if (g_checked[i] == id) return 1;
    return 0;
}

static void add_checked(int id)
{
    if (is_checked(id)) return;
    g_checked = realloc(g_checked, (g_nchecked + 1) * sizeof(int));
    g_checked[g_nchecked++] = id;
}

static void send_line(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&g_cs);
    if (g_nout < 64) {
        va_start(ap, fmt);
        vsnprintf(g_out[g_nout++], sizeof g_out[0], fmt, ap);
        va_end(ap);
    }
    LeaveCriticalSection(&g_cs);
}

static void check(int id)
{
    EnterCriticalSection(&g_cs);
    int fresh = !is_checked(id);
    if (fresh) add_checked(id);
    LeaveCriticalSection(&g_cs);
    if (fresh) {
        send_line("CHECK %d", id);
        rr_log("ap: check %d", id);
    }
}

/* shop slot <-> AP ids: boosters 0-8, U1-U8 = slots 9-16, S1-S3 = slots 19-21 */
static int slot_item(int slot)
{
    if (slot < 9) return BOOSTER_OFFSET + slot;
    if (slot < 17) return UPGRADE_OFFSET + slot - 9;
    if (slot >= 19) return INVENTION_OFFSET + slot - 19;
    return 0;
}

static int slot_location(int slot)
{
    if (slot < 9) return SHOP_OFFSET + slot;
    if (slot < 17) return SHOP_OFFSET + 100 + slot - 9;
    if (slot >= 19) return SHOP_OFFSET + 200 + slot - 19;
    return 0;
}

static int mission_id(int side, int n) { return MISSION_OFFSET + 100 * side + n; }

static int side_played(int side) { return g_side_mode == 2 || g_side_mode == side; }

/* mission 1 is always open, 2-15 need their access item, the final mission a number of wins on that side */
static int mission_open(int side, int n)
{
    if (n == 1) return 1;
    if (n < 16) return count_item(mission_id(side, n)) > 0;
    int wins = 0;
    for (int m = 1; m < 16; m++) wins += is_checked(mission_id(side, m));
    return wins >= g_final_wins;
}

static int campaign_side(void) { return *(int *)G->state == 1 ? 1 : 0; }

/* ---------------------------------------------------------------- applying the slot to the game */
static BYTE g_shop_written[SHOP_SLOTS];   /* owned[] as last written in the briefing */
static int g_shop_view;                   /* 1 = owned[] currently means "checked" (briefing) */
static int g_money_applied = -1;

static char *state_path(char *buf, size_t n)
{
    snprintf(buf, n, "%srrpatch_ap.ini", g_game_dir);
    return buf;
}

static void apply(void)
{
    BYTE *S = G->state;
    int side = campaign_side();
    if (!side_played(side)) return;   /* a campaign of the other side: leave it alone */
    DWORD mode = g_gui_mode;

    /* parts the robot builder offers */
    for (int i = 0; i < 8; i++) {
        G->heads[i] = count_item(HEAD_OFFSET + side * 8 + i) ? '+' : '-';
        G->legs[i] = count_item(LEGS_OFFSET + side * 8 + i) ? '+' : '-';
    }
    G->heads[8] = G->legs[8] = 0;
    for (int i = 0; i < 9; i++) G->boosters[i] = count_item(BOOSTER_OFFSET + i) ? '+' : '-';
    G->boosters[9] = 0;

    BYTE *avail = S + 0x0C, *owned = S + 0x22;
    if (mode == RR_MODE_BRIEFING) {
        if (g_shop_view) {
            /* a good bought since the last look is a check */
            for (int s = 0; s < SHOP_SLOTS; s++)
                if (slot_location(s) && owned[s] && !g_shop_written[s]) check(slot_location(s));
        }
        for (int s = 0; s < SHOP_SLOTS; s++) {
            int loc = slot_location(s);
            int done = loc ? is_checked(loc) : 1;
            avail[s] = loc ? 1 : 0;
            owned[s] = g_shop_written[s] = (BYTE)done;
        }
        g_shop_view = 1;
    } else {
        for (int s = 0; s < SHOP_SLOTS; s++) {
            int item = slot_item(s);
            owned[s] = item ? count_item(item) > 0 : 0;
            avail[s] = 1;
        }
        g_shop_view = 0;
    }

    /* Shop Money: added once per item; how many were added is kept per seed */
    int money = count_item(FILLER_SHOP_MONEY);
    if (g_money_applied < 0) {
        char p[MAX_PATH];
        g_money_applied = GetPrivateProfileIntA(g_seed, "shop_money_applied", 0, state_path(p, sizeof p));
    }
    if (money > g_money_applied) {
        *(int *)(S + 8) += (money - g_money_applied) * SHOP_MONEY;
        g_money_applied = money;
        char p[MAX_PATH], v[16];
        snprintf(v, sizeof v, "%d", money);
        WritePrivateProfileStringA(g_seed, "shop_money_applied", v, state_path(p, sizeof p));
    }
}

/* ---------------------------------------------------------------- for the dialogs (apdlg.c) */
int ap_side_mode(void) { return g_have_slot ? g_side_mode : -1; }
int ap_campaign_side(void) { return campaign_side(); }

int ap_mission_open(int side, int n)
{
    EnterCriticalSection(&g_cs);
    int open = g_have_slot && side_played(side) && mission_open(side, n);
    LeaveCriticalSection(&g_cs);
    return open;
}

void ap_choose_mission(int side, int n)
{
    EnterCriticalSection(&g_cs);
    g_choose_side = side;
    g_choose_mission = n;
    LeaveCriticalSection(&g_cs);
}

/* ---------------------------------------------------------------- missions */
static int g_mission_running;     /* mission (1-16) being played, 0 = none */
static int g_last_spy;

static int pick_mission(void)
{
    int cur = *(int *)(G->state + 4) + 1, side = campaign_side();
    if (g_choose_mission && g_choose_side == side && mission_open(side, g_choose_mission)) return g_choose_mission;
    if (mission_open(side, cur) && !is_checked(mission_id(side, cur))) return cur;
    for (int n = 1; n <= 16; n++)
        if (mission_open(side, n) && !is_checked(mission_id(side, n))) return n;
    return mission_open(side, cur) ? cur : 1;
}

static void on_mode(DWORD mode)
{
    if (!G || !g_have_slot) return;
    EnterCriticalSection(&g_cs);
    if (g_mission_running && mode != RR_MODE_GAMEPLAY && mode != RR_MODE_SHOP) {
        if (*G->winflag == 1) {
            LeaveCriticalSection(&g_cs);
            check(mission_id(campaign_side(), g_mission_running));
            EnterCriticalSection(&g_cs);
        }
        g_mission_running = 0;
    }
    if (mode == RR_MODE_BRIEFING || mode == RR_MODE_LOADLEVEL) {
        int n = pick_mission();
        int *level = (int *)(G->state + 4);
        if (*level != n - 1) {
            rr_log("ap: mission %d -> %d", *level + 1, n);
            *level = n - 1;
        }
        if (mode == RR_MODE_LOADLEVEL) g_choose_mission = 0;
    }
    if (mode == RR_MODE_GAMEPLAY && !g_mission_running) g_mission_running = *(int *)(G->state + 4) + 1;
    LeaveCriticalSection(&g_cs);
    send_line("STATE %lu %d", mode, *(int *)(G->state + 4) + 1);
}

/* ---------------------------------------------------------------- robots */
/* Orders only get logged: an order can still be cancelled. The checks come when the robot leaves the factory. */
void *g_order_resume;
static int g_last_order[3] = { -1, -1, -1 };
void __cdecl ap_robot_ordered(int head, int legs, int booster)
{
    g_last_order[0] = head; g_last_order[1] = legs; g_last_order[2] = booster;
    rr_log("ap: robot ordered: head %d legs %d booster %d", head, legs, booster);
}

/* entry of the order function: edx = weapon type, ecx = legs type, [esp+16] = booster (-1 = none) */
__asm__(
    ".text\n"
    ".globl _order_stub\n"
    "_order_stub:\n"
    "    pushal\n"
    "    pushl 48(%esp)\n"          /* 32 (pushal) + 16 */
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    call _ap_robot_ordered\n"
    "    addl $12, %esp\n"
    "    popal\n"
    "    pushl %esi\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    subl $16, %esp\n"
    "    jmp *_g_order_resume\n");
void order_stub(void);

/* A robot of the local player came out of the factory (the spawn code counts it for "robos produced").
   robot +0x1C weapon object, +0x20 legs object (each: +8 -> +0x18 = part type), +0x470 booster (-1 = none) */
void *g_built_resume;
DWORD *g_produced_counter;
static int part_type(BYTE *part)
{
    if (IsBadReadPtr(part, 0x0C)) return -1;
    BYTE *def = *(BYTE **)(part + 8);
    if (IsBadReadPtr(def, 0x1C)) return -1;
    return *(int *)(def + 0x18);
}

void __cdecl ap_robot_built(BYTE *robot)
{
    if (!robot || IsBadReadPtr(robot, 0x474)) return;
    /* the part types are the parts' text numbers: weapons 100-115, legs 200-215 */
    int head = part_type(*(BYTE **)(robot + 0x1C)) - 100;
    int legs = part_type(*(BYTE **)(robot + 0x20)) - 200;
    int booster = *(int *)(robot + 0x470);
    rr_log("ap: robot built: head %d legs %d booster %d (ordered %d %d %d)", head, legs, booster,
           g_last_order[0], g_last_order[1], g_last_order[2]);
    if (!g_have_slot) return;
    if (head >= 0 && head < 16) check(HEAD_OFFSET + head);
    if (legs >= 0 && legs < 16) check(LEGS_OFFSET + legs);
    if (head >= 0 && head < 16 && legs >= 0 && legs < 16) check(COMBO_OFFSET + head * 16 + legs);
    if (booster >= 0 && booster < 9) check(BOOSTER_OFFSET + booster);
}

/* replaces "inc dword [robos produced]"; esi = the new robot */
__asm__(
    ".text\n"
    ".globl _built_stub\n"
    "_built_stub:\n"
    "    pushal\n"
    "    pushl %esi\n"
    "    call _ap_robot_built\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    pushl %eax\n"
    "    movl _g_produced_counter, %eax\n"
    "    incl (%eax)\n"
    "    popl %eax\n"
    "    jmp *_g_built_resume\n");
void built_stub(void);

/* ---------------------------------------------------------------- game thread */
static void tick(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 30) return;
    last = now;
    EnterCriticalSection(&g_cs);
    if (g_have_slot) {
        apply();
        if (g_spy_price && g_gui_mode == RR_MODE_BRIEFING) {
            int p = *g_spy_price;
            /* the price is -1 before the spy names one, and again once his report is bought */
            if (p == -1 && g_last_spy > 0) {
                int n = *(int *)(G->state + 4) + 1;
                LeaveCriticalSection(&g_cs);
                check(SPY_OFFSET + 100 * campaign_side() + n);
                EnterCriticalSection(&g_cs);
            }
            g_last_spy = p;
        }
    }
    LeaveCriticalSection(&g_cs);
    apdlg_tick();
}

static BOOL (WINAPI *prev_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *prev_GetMessageA)(LPMSG, HWND, UINT, UINT);
static BOOL WINAPI ap_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { tick(); return prev_PeekMessageA(m, h, a, b, r); }
static BOOL WINAPI ap_GetMessageA(LPMSG m, HWND h, UINT a, UINT b) { tick(); return prev_GetMessageA(m, h, a, b); }

/* ---------------------------------------------------------------- the connection */
static int *parse_ids(const char *s, int *n)
{
    int cap = 64, *v = malloc(cap * sizeof(int));
    *n = 0;
    while (*s) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        if (*n == cap) v = realloc(v, (cap *= 2) * sizeof(int));
        v[(*n)++] = strtol(s, (char **)&s, 10);
        while (*s && *s != ',') s++;
    }
    return v;
}

static void handle(char *line)
{
    if (!strncmp(line, "SLOT ", 5)) {
        int side = 0, final_wins = 10;
        char seed[64] = "";
        sscanf(line + 5, "%d %d %63s", &side, &final_wins, seed);
        EnterCriticalSection(&g_cs);
        if (strcmp(seed, g_seed)) g_money_applied = -1;
        g_side_mode = side; g_final_wins = final_wins;
        lstrcpynA(g_seed, seed, sizeof g_seed);
        g_have_slot = 1;
        LeaveCriticalSection(&g_cs);
        rr_log("ap: slot side %d, final mission after %d wins, seed %s", side, final_wins, seed);
    } else if (!strncmp(line, "ITEMS", 5)) {
        int n, *v = parse_ids(line + 5, &n);
        EnterCriticalSection(&g_cs);
        free(g_items); g_items = v; g_nitems = n;
        LeaveCriticalSection(&g_cs);
        rr_log("ap: %d items", n);
    } else if (!strncmp(line, "CHECKED", 7)) {
        int n, *v = parse_ids(line + 7, &n);
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < n; i++) add_checked(v[i]);
        LeaveCriticalSection(&g_cs);
        free(v);
    } else if (!strncmp(line, "MISSION ", 8)) {
        int side = 0, n = 0;
        sscanf(line + 8, "%d %d", &side, &n);
        EnterCriticalSection(&g_cs);
        g_choose_side = side;
        g_choose_mission = n >= 1 && n <= 16 ? n : 0;
        LeaveCriticalSection(&g_cs);
        rr_log("ap: next mission %d of side %d", n, side);
    } else if (!strncmp(line, "MSG ", 4)) {
        rr_log("ap: %s", line + 4);
    }
}

static DWORD WINAPI net_thread(LPVOID unused)
{
    (void)unused;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (;;) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)g_port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
            closesocket(s);
            Sleep(2000);
            continue;
        }
        rr_log("ap: connected to the client on port %d", g_port);
        InterlockedExchange(&g_connected, 1);
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        char hello[64];
        snprintf(hello, sizeof hello, "HELLO %s\n", g_exe_name);
        send(s, hello, (int)strlen(hello), 0);
        static char buf[65536];
        int len = 0;
        for (;;) {
            int r = recv(s, buf + len, sizeof buf - 1 - len, 0);
            if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) break;
            if (r > 0) {
                len += r;
                buf[len] = 0;
                char *start = buf, *nl;
                while ((nl = strchr(start, '\n'))) {
                    *nl = 0;
                    if (nl > start && nl[-1] == '\r') nl[-1] = 0;
                    handle(start);
                    start = nl + 1;
                }
                len -= (int)(start - buf);
                memmove(buf, start, len);
                if (len >= (int)sizeof buf - 1) len = 0;   /* a line too long for the buffer */
            }
            char out[64][96];
            int nout;
            EnterCriticalSection(&g_cs);
            nout = g_nout;
            memcpy(out, g_out, sizeof out);
            g_nout = 0;
            LeaveCriticalSection(&g_cs);
            int ok = 1;
            for (int i = 0; i < nout && ok; i++) {
                char l[100];
                int n = snprintf(l, sizeof l, "%s\n", out[i]);
                ok = send(s, l, n, 0) == n;
            }
            if (!ok) break;
            Sleep(20);
        }
        rr_log("ap: client connection closed");
        InterlockedExchange(&g_connected, 0);
        closesocket(s);
        Sleep(2000);
    }
}

void ap_install(void)
{
    g_enabled = GetPrivateProfileIntA("ap", "enabled", 0, g_ini_path);
    g_port = GetPrivateProfileIntA("ap", "port", 38480, g_ini_path);
    if (!g_enabled) return;
    G = rr_game();
    if (!G || !G->winflag) { rr_log("ap: game data not found, Archipelago off"); return; }
    if (!rr_watch_modes()) { rr_log("ap: game mode switch not found, Archipelago off"); return; }
    InitializeCriticalSection(&g_cs);
    rr_on_mode(on_mode);

    BYTE *order = rr_find_unique((const BYTE *)"\x56\x55\x53\x83\xec\x10\x33\xc0\x8b\xea\x8b\xf1\x89\x04\x24\xe8", 16);
    if (order) {
        g_order_resume = order + 6;
        rr_write_jmp(order, (void *)order_stub, 6);
    } else rr_log("ap: robot order code not found, no build checks");

    /* cmp ebp,[local player]; je +8; inc [enemy produced]; jmp +6; inc [produced]; mov [esp],esi; call */
    BYTE *built = NULL;
    {
        BYTE *base = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !built; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 32;
            for (; p < end; p++)
                if (p[0] == 0x3B && p[1] == 0x2D && p[6] == 0x74 && p[7] == 0x08 && p[8] == 0xFF && p[9] == 0x05 &&
                    p[14] == 0xEB && p[15] == 0x06 && p[16] == 0xFF && p[17] == 0x05 &&
                    p[22] == 0x89 && p[23] == 0x34 && p[24] == 0x24 && p[25] == 0xE8) { built = p + 16; break; }
        }
    }
    if (built) {
        g_produced_counter = *(DWORD **)(built + 2);
        g_built_resume = built + 6;
        rr_write_jmp(built, (void *)built_stub, 6);
    } else rr_log("ap: robot spawn code not found, no build checks");

    BYTE *spy = rr_find_unique((const BYTE *)"\x2b\xc2\xba\xff\xff\xff\xff\x51\x89\x15", 10);
    if (spy && *(BYTE **)(spy + 15) == G->state + 8) g_spy_price = *(int **)(spy + 10);
    else rr_log("ap: spy code not found, no spy checks");

    HMODULE exe = GetModuleHandleA(NULL);
    prev_PeekMessageA = rr_hook_iat(exe, "USER32.dll", "PeekMessageA", (void *)ap_PeekMessageA);
    prev_GetMessageA = rr_hook_iat(exe, "USER32.dll", "GetMessageA", (void *)ap_GetMessageA);
    CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, NULL));
    rr_log("ap: waiting for the client on port %d (order %p, built %p, spy price %p)", g_port, order, built, g_spy_price);
}
