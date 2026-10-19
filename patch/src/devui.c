/* Developer window ([dev] enabled=1 in rrpatch.ini).

   A small tool window next to the game for testing: campaign side/level/money, win/lose the
   mission, part and shop unlocks, the game's own console commands and every adjust.ini
   value, live.

   Nothing is hard-coded per exe. The game keeps a table of its console commands
   { name, enabled, handler } and the handlers name the globals this window needs:
     dgd               "Side %c Level %d Money %d" -> campaign state block
     enable_all_parts  copies "++++++++" into the Heads and Legs masks (Boosters is 0x20 after Heads)
     restartGame       sets the restart flag
     showVals          walks the adjustables list
   The win/lose flag is the one the Shift+W+N / Shift+L+S developer keys set.
   Changes are queued and applied on the game thread (inside its PeekMessage loop). */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- located game data ---- */
typedef struct { const char *name; DWORD enabled; void (__cdecl *fn)(const char *args); } ConsoleCmd;
static ConsoleCmd *g_cmds;        /* console command table */
static int g_ncmds;
static BYTE *g_state;             /* +0 side, +4 level, +8 money, +0x0C available[22], +0x22 owned[22] */
static char *g_heads, *g_legs, *g_boosters;
static BYTE *g_restart;
static DWORD *g_winflag;          /* 1 = win the mission, 2 = lose it */
static void **g_adj_head;         /* adjustables: node[2] next, node[3] name, node[4] value pointer */

#define SLOTS 22                  /* 9 items, 10 upgrades, 3 secrets (as dgd prints them) */

static BYTE *image_base(void) { return (BYTE *)GetModuleHandleA(NULL); }

static int in_image(const void *p)
{
    BYTE *b = image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    return (BYTE *)p >= b && (BYTE *)p < b + nt->OptionalHeader.SizeOfImage;
}

static BYTE *find_bytes(const void *pat, size_t len, BYTE *from)
{
    BYTE *b = image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    BYTE *end = b + nt->OptionalHeader.SizeOfImage - len;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        BYTE *p = b + sec->VirtualAddress, *e = p + sec->Misc.VirtualSize;
        if (e > end) e = end;
        if (from > p) p = from;
        for (; p < e; p++)
            if (*p == *(const BYTE *)pat && memcmp(p, pat, len) == 0) return p;
    }
    return NULL;
}

static void *cmd_handler(const char *name)
{
    for (int i = 0; i < g_ncmds; i++)
        if (lstrcmpiA(g_cmds[i].name, name) == 0) return (void *)g_cmds[i].fn;
    return NULL;
}

/* first imm32 after `opcode` (1 or 2 bytes) within `range` bytes of a function */
static DWORD operand_after(const BYTE *fn, const BYTE *op, int oplen, int range, int nth)
{
    if (!fn) return 0;
    for (int i = 0; i < range; i++)
        if (memcmp(fn + i, op, oplen) == 0 && nth-- == 0) return *(const DWORD *)(fn + i + oplen);
    return 0;
}

static int locate(void)
{
    /* the command table: { "list", 1, handler } is its first entry */
    BYTE *from = NULL;
    for (;;) {
        BYTE *str = find_bytes("list", 5, from);
        if (!str) return 0;
        DWORD key[2] = { (DWORD)str, 1 };
        BYTE *entry = find_bytes(key, 8, NULL);
        if (entry) {
            g_cmds = (ConsoleCmd *)entry;
            break;
        }
        from = str + 1;
    }
    for (g_ncmds = 0; g_ncmds < 200; g_ncmds++) {
        ConsoleCmd *c = &g_cmds[g_ncmds];
        if (!in_image(c->name) || c->enabled > 1 || !in_image((void *)c->fn)) break;
    }
    static const BYTE mov_edx[] = { 0x8B, 0x15 }, mov_esi = 0xBE, mov_ebx[] = { 0x8B, 0x1D };
    g_state = (BYTE *)operand_after(cmd_handler("dgd"), mov_edx, 2, 64, 0);
    g_heads = (char *)operand_after(cmd_handler("enable_all_parts"), &mov_esi, 1, 32, 0);
    g_legs = (char *)operand_after(cmd_handler("enable_all_parts"), &mov_esi, 1, 32, 1);
    g_boosters = g_heads ? g_heads + 0x20 : NULL;
    const BYTE *rg = cmd_handler("restartGame");
    if (rg && rg[0] == 0xB0 && rg[1] == 0x01 && rg[2] == 0xA2) g_restart = *(BYTE **)(rg + 3);
    g_adj_head = (void **)operand_after(cmd_handler("showVals"), mov_ebx, 2, 16, 0);
    /* mov eax,4Eh ('N'); mov [esp],eax; call key; test eax,eax; jz; mov dword [flag],1 */
    for (BYTE *p = NULL;;) {
        static const BYTE head[] = { 0xB8, 0x4E, 0, 0, 0, 0x89, 0x04, 0x24, 0xE8 };
        p = find_bytes(head, sizeof head, p ? p + 1 : NULL);
        if (!p) break;
        if (p[13] == 0x85 && p[14] == 0xC0 && p[15] == 0x74 && p[17] == 0xC7 && p[18] == 0x05 &&
            *(DWORD *)(p + 23) == 1) {
            g_winflag = *(DWORD **)(p + 19);
            break;
        }
    }
    rr_log("dev: %d console commands, state %p, heads %p, legs %p, restart %p, win %p, adjustables %p",
           g_ncmds, g_state, g_heads, g_legs, g_restart, g_winflag, g_adj_head);
    return g_cmds && g_state && g_heads && g_legs;
}

const RRGame *rr_game(void)
{
    static int tried, ok;
    static RRGame g;
    if (!tried) {
        tried = 1;
        ok = locate();
        g.state = g_state; g.heads = g_heads; g.legs = g_legs; g.boosters = g_boosters;
        g.winflag = g_winflag; g.restart = g_restart;
    }
    return ok ? &g : NULL;
}

/* ---- adjustables ---- */
typedef void (__thiscall *adj_format_t)(void *self, char *out);

static void **adj_find(const char *name)
{
    if (!g_adj_head) return NULL;
    for (void **n = (void **)*g_adj_head; n; n = (void **)n[2])
        if (n[3] && lstrcmpiA((const char *)n[3], name) == 0) return n;
    return NULL;
}

static void adj_format(void **node, char *out)
{
    adj_format_t f = (adj_format_t)((void **)node[0])[1];
    out[0] = 0;
    f(node, out);
}

/* ---- actions, run on the game thread ---- */
enum { A_WIN, A_LOSE, A_RESTART, A_ALL_PARTS, A_SET_INT, A_SET_BYTE, A_SET_CHAR, A_CONSOLE, A_ADJUST, A_UNLOCK_ALL };
typedef struct { int kind; void *addr; int value; char text[256]; } Action;
#define MAXQ 64
static Action g_queue[MAXQ];
static int g_qn;
static CRITICAL_SECTION g_qcs;
static char g_result[512];        /* last console/adjust result, shown in the window */

static void queue(int kind, void *addr, int value, const char *text)
{
    EnterCriticalSection(&g_qcs);
    if (g_qn < MAXQ) {
        Action *a = &g_queue[g_qn++];
        a->kind = kind; a->addr = addr; a->value = value;
        lstrcpynA(a->text, text ? text : "", sizeof a->text);
    }
    LeaveCriticalSection(&g_qcs);
}

static void set_adjustable(const char *line)
{
    char name[128];
    const char *eq = strchr(line, '=');
    if (!eq || eq - line >= (int)sizeof name) { lstrcpyA(g_result, "use name=value"); return; }
    memcpy(name, line, eq - line);
    name[eq - line] = 0;
    void **n = adj_find(name);
    if (!n) { snprintf(g_result, sizeof g_result, "no adjustable '%s'", name); return; }
    char cur[256];
    adj_format(n, cur);
    const char *v = strchr(cur, '=');
    if (v && strchr(v, ',')) { snprintf(g_result, sizeof g_result, "%s: colour values not supported", name); return; }
    if (v && strchr(v, '.')) *(float *)n[4] = (float)atof(eq + 1);
    else *(int *)n[4] = atoi(eq + 1);
    adj_format(n, g_result);
    rr_log("dev: %s", g_result);
}

static void run_action(Action *a)
{
    switch (a->kind) {
    case A_WIN: if (g_winflag) *g_winflag = 1; rr_log("dev: win mission"); break;
    case A_LOSE: if (g_winflag) *g_winflag = 2; rr_log("dev: lose mission"); break;
    case A_RESTART: if (g_restart) *g_restart = 1; rr_log("dev: restart"); break;
    case A_SET_INT: *(int *)a->addr = a->value; break;
    case A_SET_BYTE: *(BYTE *)a->addr = (BYTE)a->value; break;
    case A_SET_CHAR: *(char *)a->addr = (char)a->value; break;
    case A_ALL_PARTS:
    case A_UNLOCK_ALL: {
        void (__cdecl *f)(const char *) = cmd_handler("enable_all_parts");
        if (f) f("");
        if (g_boosters) for (int i = 0; i < 9; i++) g_boosters[i] = '+';
        if (a->kind == A_UNLOCK_ALL) for (int i = 0; i < SLOTS; i++) g_state[0x0C + i] = 1;
        rr_log("dev: %s", a->kind == A_ALL_PARTS ? "all parts" : "unlock everything");
        break;
    }
    case A_CONSOLE: {
        char line[256], *args;
        lstrcpynA(line, a->text, sizeof line);
        args = strchr(line, ' ');
        if (args) *args++ = 0; else args = line + strlen(line);
        void (__cdecl *f)(const char *) = cmd_handler(line);
        if (f) {
            f(args);
            snprintf(g_result, sizeof g_result, "ran '%s %s' (output is in the in-game console: Shift+Ctrl+F11)", line, args);
        } else if (strchr(a->text, '=')) set_adjustable(a->text);
        else snprintf(g_result, sizeof g_result, "unknown command '%s'", line);
        rr_log("dev: console %s", a->text);
        break;
    }
    case A_ADJUST: set_adjustable(a->text); break;
    }
}

static void run_queue(void)
{
    if (!g_qn) return;
    EnterCriticalSection(&g_qcs);
    Action local[MAXQ];
    int n = g_qn;
    memcpy(local, g_queue, n * sizeof(Action));
    g_qn = 0;
    LeaveCriticalSection(&g_qcs);
    for (int i = 0; i < n; i++) run_action(&local[i]);
}

static BOOL (WINAPI *real_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *real_GetMessageA)(LPMSG, HWND, UINT, UINT);
static BOOL WINAPI hook_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { run_queue(); return real_PeekMessageA(m, h, a, b, r); }
static BOOL WINAPI hook_GetMessageA(LPMSG m, HWND h, UINT a, UINT b) { run_queue(); return real_GetMessageA(m, h, a, b); }

/* ---- the window ---- */
static const char *k_heads[2][8] = {
    { "Chain Gun", "Rocket Launcher", "Homing Missile", "Mine Spreader", "Oiler", "Magneton", "Force Field", "Nuke" },
    { "Laser", "Pulse Gun", "Eraser", "Buzzer", "Nitrogrenade", "Disrupter", "Whirlwind", "Infector" },
};
static const char *k_legs[2][8] = {
    { "2-Legs A", "Heavy 4-Legs", "Kangaroo", "Caterpillar", "4-Tires", "Amphibian", "Propeller", "Snail" },
    { "2-Legs B", "Teleporter", "Jet-Pack", "Spider Legs", "Jet-Car", "Hovercraft", "Antigraviton", "Cube" },
};
/* Boosters mask order, as the shop lists them. The last three are pick-two-of-three. */
static const char *k_boosters[9] = { "Speed", "Turn Speed", "Fire Power", "Fire Range", "Fire Frequency", "Shield",
                                     "Camouflage", "Maximum Weapon", "Maximum Defense" };

enum {
    ID_STATE = 100, ID_SIDE, ID_LEVEL, ID_MONEY, ID_SET_SIDE, ID_SET_LEVEL, ID_SET_MONEY,
    ID_WIN, ID_LOSE, ID_RESTART, ID_ALLPARTS, ID_UNLOCKALL, ID_MONEY_PLUS,
    ID_CONSOLE, ID_RUN, ID_RESULT, ID_ADJ_PATTERN, ID_ADJ_LIST, ID_ADJ_OUT,
    ID_HEAD0 = 200, ID_LEG0 = 220, ID_BOOST0 = 240, ID_AVAIL0 = 260, ID_OWNED0 = 300,
};
static HWND g_dev;
static HFONT g_font;

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_dev, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

/* side is stored as 0 = A, 1 = B */
static int side_index(void) { int v = g_state ? *(int *)g_state : 0; return v == 1 || v == 'B' ? 1 : 0; }

static void build_controls(void)
{
    int y = 8;
    add("STATIC", "", SS_LEFT, 8, y, 600, 18, ID_STATE);
    y += 24;
    add("STATIC", "Level (1-16)", 0, 110, y + 3, 84, 18, 0);
    add("EDIT", "", ES_AUTOHSCROLL | ES_NUMBER, 196, y, 40, 22, ID_LEVEL);
    add("BUTTON", "Set", BS_PUSHBUTTON, 238, y, 40, 22, ID_SET_LEVEL);
    add("STATIC", "Money", 0, 296, y + 3, 38, 18, 0);
    add("EDIT", "", ES_AUTOHSCROLL | ES_NUMBER, 336, y, 90, 22, ID_MONEY);
    add("BUTTON", "Set", BS_PUSHBUTTON, 428, y, 40, 22, ID_SET_MONEY);
    add("BUTTON", "+1,000,000", BS_PUSHBUTTON, 474, y, 90, 22, ID_MONEY_PLUS);
    y += 30;
    add("BUTTON", "Win mission", BS_PUSHBUTTON, 8, y, 100, 26, ID_WIN);
    add("BUTTON", "Lose mission", BS_PUSHBUTTON, 112, y, 100, 26, ID_LOSE);
    add("BUTTON", "Restart", BS_PUSHBUTTON, 216, y, 80, 26, ID_RESTART);
    add("BUTTON", "All parts + boosters", BS_PUSHBUTTON, 300, y, 140, 26, ID_ALLPARTS);
    add("BUTTON", "Unlock everything", BS_PUSHBUTTON, 444, y, 120, 26, ID_UNLOCKALL);
    y += 34;
    add("BUTTON", "Heads (weapons)", BS_GROUPBOX, 8, y, 180, 196, 0);
    add("BUTTON", "Legs (chassis)", BS_GROUPBOX, 196, y, 180, 196, 0);
    add("BUTTON", "Boosters", BS_GROUPBOX, 384, y, 180, 196, 0);
    for (int i = 0; i < 8; i++) {
        add("BUTTON", "", BS_AUTOCHECKBOX, 16, y + 18 + i * 21, 168, 20, ID_HEAD0 + i);
        add("BUTTON", "", BS_AUTOCHECKBOX, 204, y + 18 + i * 21, 168, 20, ID_LEG0 + i);
    }
    for (int i = 0; i < 9; i++)
        add("BUTTON", k_boosters[i], BS_AUTOCHECKBOX, 392, y + 18 + i * 19, 168, 18, ID_BOOST0 + i);
    y += 204;
    add("STATIC", "Shop slots: I1-I9 = the boosters above, U = upgrades, S = secrets", 0, 8, y, 556, 18, 0);
    y += 18;
    add("STATIC", "available", 0, 8, y + 2, 56, 18, 0);
    add("STATIC", "owned", 0, 8, y + 24, 56, 18, 0);
    for (int i = 0; i < SLOTS; i++) {
        char label[8];
        if (i < 9) snprintf(label, sizeof label, "I%d", i + 1);
        else if (i < 19) snprintf(label, sizeof label, "U%d", i - 8);
        else snprintf(label, sizeof label, "S%d", i - 18);
        int x = 66 + i * 23;
        add("STATIC", label, SS_CENTER, x - 2, y - 16 + 0, 24, 14, 0);
        add("BUTTON", "", BS_AUTOCHECKBOX, x + 3, y, 18, 20, ID_AVAIL0 + i);
        add("BUTTON", "", BS_AUTOCHECKBOX, x + 3, y + 22, 18, 20, ID_OWNED0 + i);
    }
    y += 50;
    add("STATIC", "Console command or name=value", 0, 8, y, 300, 18, 0);
    y += 18;
    add("EDIT", "", ES_AUTOHSCROLL, 8, y, 480, 22, ID_CONSOLE);
    add("BUTTON", "Run", BS_DEFPUSHBUTTON, 492, y, 72, 22, ID_RUN);
    y += 26;
    add("STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 8, y, 556, 18, ID_RESULT);
    y += 24;
    add("STATIC", "Adjustables matching", 0, 8, y + 3, 120, 18, 0);
    add("EDIT", "zoom", ES_AUTOHSCROLL, 130, y, 280, 22, ID_ADJ_PATTERN);
    add("BUTTON", "List", BS_PUSHBUTTON, 414, y, 72, 22, ID_ADJ_LIST);
    y += 26;
    add("EDIT", "", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 8, y, 556, 150, ID_ADJ_OUT);
}

static void refresh(void)
{
    if (!g_state) return;
    char buf[160];
    int side = side_index();
    snprintf(buf, sizeof buf, "Side %c   Level %d   Money %d   (a new level takes effect on Restart)",
             side ? 'B' : 'A', *(int *)(g_state + 4), *(int *)(g_state + 8));
    SetDlgItemTextA(g_dev, ID_STATE, buf);
    for (int i = 0; i < 8; i++) {
        SetDlgItemTextA(g_dev, ID_HEAD0 + i, k_heads[side][i]);
        SetDlgItemTextA(g_dev, ID_LEG0 + i, k_legs[side][i]);
        CheckDlgButton(g_dev, ID_HEAD0 + i, g_heads[i] == '+' ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_dev, ID_LEG0 + i, g_legs[i] == '+' ? BST_CHECKED : BST_UNCHECKED);
    }
    for (int i = 0; i < 9; i++)
        CheckDlgButton(g_dev, ID_BOOST0 + i, g_boosters[i] == '+' ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < SLOTS; i++) {
        CheckDlgButton(g_dev, ID_AVAIL0 + i, g_state[0x0C + i] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_dev, ID_OWNED0 + i, g_state[0x22 + i] ? BST_CHECKED : BST_UNCHECKED);
    }
    char cur[sizeof g_result];
    GetDlgItemTextA(g_dev, ID_RESULT, cur, sizeof cur);
    if (strcmp(cur, g_result) != 0) SetDlgItemTextA(g_dev, ID_RESULT, g_result);
}

static void list_adjustables(void)
{
    char pattern[128];
    GetDlgItemTextA(g_dev, ID_ADJ_PATTERN, pattern, sizeof pattern);
    CharLowerA(pattern);
    size_t cap = 65536, len = 0;
    char *out = malloc(cap);
    if (!out || !g_adj_head) { free(out); return; }
    out[0] = 0;
    int count = 0;
    for (void **n = (void **)*g_adj_head; n && count < 2000; n = (void **)n[2]) {
        char name[128];
        lstrcpynA(name, (const char *)n[3], sizeof name);
        CharLowerA(name);
        if (pattern[0] && !strstr(name, pattern)) continue;
        char line[256];
        adj_format(n, line);
        int w = snprintf(out + len, cap - len, "%s\r\n", line);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += w;
        count++;
    }
    SetDlgItemTextA(g_dev, ID_ADJ_OUT, out);
    free(out);
}

static int edit_int(int id) { char b[32]; GetDlgItemTextA(g_dev, id, b, sizeof b); return atoi(b); }

static LRESULT CALLBACK dev_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER: refresh(); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        char text[256];
        if (id >= ID_HEAD0 && id < ID_HEAD0 + 8)
            queue(A_SET_CHAR, g_heads + (id - ID_HEAD0), IsDlgButtonChecked(h, id) ? '+' : '-', NULL);
        else if (id >= ID_LEG0 && id < ID_LEG0 + 8)
            queue(A_SET_CHAR, g_legs + (id - ID_LEG0), IsDlgButtonChecked(h, id) ? '+' : '-', NULL);
        else if (id >= ID_BOOST0 && id < ID_BOOST0 + 9)
            queue(A_SET_CHAR, g_boosters + (id - ID_BOOST0), IsDlgButtonChecked(h, id) ? '+' : '-', NULL);
        else if (id >= ID_AVAIL0 && id < ID_AVAIL0 + SLOTS)
            queue(A_SET_BYTE, g_state + 0x0C + (id - ID_AVAIL0), IsDlgButtonChecked(h, id) ? 1 : 0, NULL);
        else if (id >= ID_OWNED0 && id < ID_OWNED0 + SLOTS)
            queue(A_SET_BYTE, g_state + 0x22 + (id - ID_OWNED0), IsDlgButtonChecked(h, id) ? 1 : 0, NULL);
        else switch (id) {
        case ID_SET_LEVEL: {
            int lv = edit_int(ID_LEVEL);
            if (lv < 1 || lv > 16) { lstrcpyA(g_result, "level must be 1-16 (higher crashes the game)"); break; }
            queue(A_SET_INT, g_state + 4, lv, NULL);
            break;
        }
        case ID_SET_MONEY: queue(A_SET_INT, g_state + 8, edit_int(ID_MONEY), NULL); break;
        case ID_MONEY_PLUS: queue(A_SET_INT, g_state + 8, *(int *)(g_state + 8) + 1000000, NULL); break;
        case ID_WIN: queue(A_WIN, NULL, 0, NULL); break;
        case ID_LOSE: queue(A_LOSE, NULL, 0, NULL); break;
        case ID_RESTART: queue(A_RESTART, NULL, 0, NULL); break;
        case ID_ALLPARTS: queue(A_ALL_PARTS, NULL, 0, NULL); break;
        case ID_UNLOCKALL: queue(A_UNLOCK_ALL, NULL, 0, NULL); break;
        case ID_RUN: case IDOK:
            GetDlgItemTextA(h, ID_CONSOLE, text, sizeof text);
            if (text[0]) queue(A_CONSOLE, NULL, 0, text);
            break;
        case ID_ADJ_LIST: list_adjustables(); break;
        }
        return 0;
    }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;   /* the game owns the process */
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI dev_thread(LPVOID arg)
{
    (void)arg;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = dev_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "RoboRumbleDev";
    RegisterClassA(&wc);
    NONCLIENTMETRICSA ncm = { sizeof ncm };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    RECT r = { 0, 0, 572, 740 };
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_TOOLWINDOW);
    g_dev = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "RoboRumble dev",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            20, 20, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    build_controls();
    ShowWindow(g_dev, SW_SHOWNOACTIVATE);
    SetTimer(g_dev, 1, 300, NULL);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_dev, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    return 0;
}

void devui_install(void)
{
    if (!GetPrivateProfileIntA("dev", "enabled", 0, g_ini_path)) return;
    if (!rr_game()) { rr_log("dev: game data not found, dev window off"); return; }
    InitializeCriticalSection(&g_qcs);
    HMODULE exe = GetModuleHandleA(NULL);
    real_PeekMessageA = rr_hook_iat(exe, "USER32.dll", "PeekMessageA", (void *)hook_PeekMessageA);
    real_GetMessageA = rr_hook_iat(exe, "USER32.dll", "GetMessageA", (void *)hook_GetMessageA);
    if (!real_PeekMessageA) { rr_log("dev: PeekMessageA not hooked, dev window off"); return; }
    CloseHandle(CreateThread(NULL, 0, dev_thread, NULL, 0, NULL));
}
