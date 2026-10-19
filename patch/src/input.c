/* Mission controls ([input] in rrpatch.ini).

   The game's own battlefield mouse handling (FUN_00406430 down / FUN_00406740 up in rr_soft):
     left down    starts a click; dragging turns it into a selection box
     left up      own robot -> select it (Ctrl toggles), enemy -> attack it with the selection,
                  empty ground -> move the selection there
     right down   deselect everything
   Clicks on the panels are taken by the interface before any of this.

   right_click_commands=1 turns this into the usual RTS layout:
     left         select / drag a box, as before; on an enemy it only selects it, on empty
                  ground it deselects (what the right button used to do)
     right click  the left button's old click on the battlefield: move / attack
   right_drag_pan=1: holding the right button and moving the mouse pans the camera instead.
   wasd_camera=1: W A S D scroll the camera like the arrow keys during missions (the game's
   own S = shadows and D = screenshot still work with Shift held).

   The game code is patched at a few points found by byte patterns (same code in rr_soft,
   rr_dx5 and rr_glide of both releases):
     down         after the UI check: right button -> run the left button's click start,
                  remembering which button it was
     enemy        "units selected -> attack" check, skipped for left clicks
     ground       the call that turns a ground click into a move order; for left clicks it
                  deselects instead
     arrows       right after the arrow keys set the scroll speeds; camera panning adds to them
   The game mode (GUI_MODE_*, GAMEPLAY = 4) is taken from the main loop's "New mode is %s"
   log call, so WASD only changes meaning during missions. */
#include "common.h"
#include <math.h>
#include <stdlib.h>

#define GUI_MODE_GAMEPLAY 4

volatile DWORD g_gui_mode = 1;      /* LOADING until the first switch */
void *g_mode_resume;
DWORD g_mode_fmt;                    /* address of "New mode is %s...\n" */

/* shared with the asm stubs */
BYTE g_click_left;                   /* the click being handled came from the left button */
BYTE g_rclick_armed;                 /* right button went down on the battlefield */
void *g_down_left, *g_down_right;    /* button-down handler: left path / original right path */
void *g_enemy_attack, *g_enemy_select;
void *g_ground_orig;                 /* screen -> map position, start of the move order */
void *g_arrows_resume;
float *g_vel[4];                     /* scroll speeds: left, right, up, down */
float *g_vel_min;                    /* speeds below this are dropped by the game */

static BYTE *g_click_pending;        /* the game's "button is down, click in progress" flag */
static DWORD *g_unit_list;           /* head of the robot list: +8 next, +0x39d selected */
static void (__cdecl *g_play_sound)(int, int, int, int);

static int g_right_cmds, g_pan, g_wasd;
static float g_pan_speed = 1.0f;
static int g_pan_grab;

__asm__(
    ".text\n"
    ".globl _mode_stub\n"
    "_mode_stub:\n"
    "    movl %ebx, _g_gui_mode\n"
    "    pushal\n"
    "    pushl %ebx\n"
    "    call _rr_mode_changed\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    movl _g_mode_fmt, %eax\n"
    "    jmp *_g_mode_resume\n"

    /* replaces "test ebx, 2 / jne right_path"; ebx = MK_* flags of the button-down message */
    ".globl _down_stub\n"
    "_down_stub:\n"
    "    testl $2, %ebx\n"
    "    jnz 1f\n"
    "    movb $1, _g_click_left\n"
    "    jmp *_g_down_left\n"
    "1:  movb $0, _g_click_left\n"
    "    movb $1, _g_rclick_armed\n"
    "    andl $0xFFFFFFFD, %ebx\n"
    "    orl $1, %ebx\n"
    "    jmp *_g_down_left\n"

    /* replaces "test al, al / jne attack / mov edx, esi"; al = own robots are selected */
    ".globl _enemy_stub\n"
    "_enemy_stub:\n"
    "    cmpb $0, _g_click_left\n"
    "    je 1f\n"
    "    xorb %al, %al\n"
    "1:  movl %esi, %edx\n"
    "    testb %al, %al\n"
    "    jne 2f\n"
    "    jmp *_g_enemy_select\n"
    "2:  jmp *_g_enemy_attack\n"

    /* called instead of the screen -> map conversion that starts a move order */
    ".globl _ground_stub\n"
    "_ground_stub:\n"
    "    cmpb $0, _g_click_left\n"
    "    jne 1f\n"
    "    jmp *_g_ground_orig\n"
    "1:  pushal\n"
    "    call _ground_left_click\n"
    "    popal\n"
    "    xorl %eax, %eax\n"
    "    ret\n"

    /* replaces "fld dword [left speed]" after the arrow keys */
    ".globl _arrows_stub\n"
    "_arrows_stub:\n"
    "    pushal\n"
    "    call _pan_apply\n"
    "    popal\n"
    "    movl _g_vel, %eax\n"
    "    flds (%eax)\n"
    "    jmp *_g_arrows_resume\n");
void mode_stub(void);

/* Runs on the game thread right after the main loop picked its next screen, before that screen starts. */
static void (*g_mode_listeners[4])(DWORD mode);
void rr_on_mode(void (*fn)(DWORD mode))
{
    for (int i = 0; i < 4; i++) if (!g_mode_listeners[i]) { g_mode_listeners[i] = fn; return; }
}

void __cdecl rr_mode_changed(DWORD mode)
{
    for (int i = 0; i < 4; i++) if (g_mode_listeners[i]) g_mode_listeners[i](mode);
}
void down_stub(void);
void enemy_stub(void);
void ground_stub(void);
void arrows_stub(void);

/* Left click on empty ground: deselect everything, like the right button used to. */
void ground_left_click(void)
{
    int any = 0;
    for (BYTE *u = (BYTE *)*g_unit_list; u; u = *(BYTE **)(u + 8)) {
        any |= u[0x39d];
        u[0x39d] = 0;
    }
    if (any) g_play_sound(99, 0, 0, 0);
}

/* ---------------------------------------------------------------- right-drag camera pan */
static int g_rdown, g_panning;
static POINT g_anchor;
static float g_acc_x, g_acc_y;      /* game pixels moved since the last game tick */

void pan_apply(void)
{
    if (g_acc_x == 0 && g_acc_y == 0) return;
    float sx = g_acc_x * g_pan_speed, sy = g_acc_y * g_pan_speed;
    if (g_pan_grab) { sx = -sx; sy = -sy; }
    float min = *g_vel_min;
    if (fabsf(sx) >= min) { *g_vel[sx < 0 ? 0 : 1] += fabsf(sx); g_acc_x = 0; }
    if (fabsf(sy) >= min) { *g_vel[sy < 0 ? 2 : 3] += fabsf(sy); g_acc_y = 0; }
}

static void pan_end(void)
{
    if (g_panning && g_click_pending) *g_click_pending = 0;   /* no click when the button comes up */
    g_rdown = g_panning = 0;
    g_rclick_armed = 0;
}

/* Called for every message the game takes out of the queue. Returns 1 to hide it. */
static int pan_message(MSG *m)
{
    switch (m->message) {
    case WM_RBUTTONDOWN:
        g_rdown = 1;
        g_panning = 0;
        g_rclick_armed = 0;          /* set again by the game code if it was on the battlefield */
        GetCursorPos(&g_anchor);
        return 0;
    case WM_RBUTTONUP:
        if (g_rdown) pan_end();
        return 0;
    case WM_MOUSEMOVE: {
        if (!g_rdown || !g_rclick_armed) return 0;
        if (!(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {   /* released outside the window */
            g_panning = 1;
            pan_end();
            return 0;
        }
        POINT p;
        GetCursorPos(&p);
        int dx = p.x - g_anchor.x, dy = p.y - g_anchor.y;
        if (!g_panning && abs(dx) + abs(dy) < 6) return 1;   /* still a click */
        g_panning = 1;
        RECT c;
        HWND h = rr_game_hwnd();
        float scale = (h && GetClientRect(h, &c) && c.right > 0) ? 640.0f / c.right : 0.5f;
        g_acc_x += dx * scale;
        g_acc_y += dy * scale;
        SetCursorPos(g_anchor.x, g_anchor.y);
        return 1;
    }
    }
    return 0;
}

/* ---------------------------------------------------------------- WASD */
static WPARAM g_wasd_down[4];        /* what each of W A S D was sent as when pressed */

static int wasd_index(WPARAM vk)
{
    switch (vk) { case 'W': return 0; case 'A': return 1; case 'S': return 2; case 'D': return 3; }
    return -1;
}

static void wasd_message(MSG *m)
{
    if (m->message != WM_KEYDOWN && m->message != WM_KEYUP) return;
    int i = wasd_index(m->wParam);
    if (i < 0) return;
    static const WPARAM arrow[4] = { VK_UP, VK_LEFT, VK_DOWN, VK_RIGHT };
    if (m->message == WM_KEYDOWN) {
        int plain = g_gui_mode == GUI_MODE_GAMEPLAY && !(GetKeyState(VK_SHIFT) & 0x8000) &&
                    !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000);
        if (!g_wasd_down[i]) g_wasd_down[i] = plain ? arrow[i] : m->wParam;
        m->wParam = g_wasd_down[i];
    } else {
        if (g_wasd_down[i]) m->wParam = g_wasd_down[i];
        g_wasd_down[i] = 0;
    }
}

/* ---------------------------------------------------------------- message hooks */
static void log_mode(void)
{
    static DWORD seen = (DWORD)-1;
    if (g_gui_mode != seen) {
        seen = g_gui_mode;
        rr_log("input: game mode %lu", seen);
    }
}

static void filter(MSG *m)
{
    log_mode();
    if (g_pan && pan_message(m)) {
        m->message = WM_NULL;
        m->wParam = 0;
        m->lParam = 0;
        return;
    }
    if (g_wasd) wasd_message(m);
}

static BOOL (WINAPI *prev_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *prev_GetMessageA)(LPMSG, HWND, UINT, UINT);

static BOOL WINAPI in_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r)
{
    BOOL got = prev_PeekMessageA(m, h, a, b, r);
    if (got && (r & PM_REMOVE)) filter(m);
    return got;
}

static BOOL WINAPI in_GetMessageA(LPMSG m, HWND h, UINT a, UINT b)
{
    BOOL got = prev_GetMessageA(m, h, a, b);
    if (got > 0) filter(m);
    return got;
}

/* ---------------------------------------------------------------- finding the code */
/* pattern with '?' wildcards given as a mask string: 'x' = must match, '?' = any */
static BYTE *find_masked(const char *pat, const char *mask)
{
    size_t len = strlen(mask);
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *found = NULL;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - len;
        for (; p <= end; p++) {
            size_t i = 0;
            while (i < len && (mask[i] == '?' || p[i] == (BYTE)pat[i])) i++;
            if (i < len) continue;
            if (found) return NULL;
            found = p;
        }
    }
    return found;
}

static int hook_mode_switch(void);
static BYTE *rel32(BYTE *at) { return at + 4 + *(int *)at; }   /* target of a rel32 operand */

int rr_watch_modes(void)
{
    static int done = -1;
    if (done < 0) done = hook_mode_switch();
    return done;
}

static int hook_mode_switch(void)
{
    /* mov eax, offset fmt / mov edx, [ebx*4 + table] / mov [esp+4], edx / mov [esp], eax / call */
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 20;
        for (; p < end; p++) {
            if (p[0] != 0xB8 || p[5] != 0x8B || p[6] != 0x14 || p[7] != 0x9D) continue;
            if (memcmp(p + 12, "\x89\x54\x24\x04\x89\x04\x24\xE8", 8) != 0) continue;
            const char *fmt = *(const char **)(p + 1);
            if (IsBadReadPtr(fmt, 12) || strncmp(fmt, "New mode is", 11) != 0) continue;
            g_mode_fmt = (DWORD)fmt;
            g_mode_resume = p + 5;
            return rr_write_jmp(p, (void *)mode_stub, 5);
        }
    }
    return 0;
}

static int install_clicks(void)
{
    BYTE *down = find_masked("\xf7\xc3\x02\x00\x00\x00\x0f\x85????\xb0\x01\xba????\x89\x14\x24\x66\x89\x1d????\xa2",
                             "xxxxxxxx????xxx????xxxxxx????x");
    BYTE *enemy = find_masked("\x84\xc0\x75?\x8b\xd6\x85\xf6\x74?\xb0\x00\x88\x82\x9d\x03\x00\x00",
                              "xxx?xxxxx?xxxxxxxx");
    BYTE *ground = find_masked("\x89\x44\x24\x18\x8b\xc2\x8b\xd1\x8b\x0d????\xe8????\x85\xc0\x0f\x84",
                               "xxxxxxxxxx????x????xxxx");
    BYTE *desel = find_masked("\x8b\x0d????\xb4\x00\x85\xc9\x74?\xb2\x00\x8a\x81\x9d\x03\x00\x00\x88\x91\x9d\x03\x00\x00",
                              "xx????xxxxx?xxxxxxxxxxxxxx");
    if (!down || !enemy || !ground || !desel) {
        rr_log("input: battlefield click code not found (%p %p %p %p)", down, enemy, ground, desel);
        return 0;
    }
    BYTE *snd = NULL;
    for (int i = 0; i < 0x60; i++)
        if (memcmp(desel + i, "\x89\x54\x24\x0c\xe8", 5) == 0) { snd = rel32(desel + i + 5); break; }
    if (!snd) { rr_log("input: deselect sound not found"); return 0; }

    g_down_left = down + 12;
    g_down_right = rel32(down + 8);
    g_click_pending = *(BYTE **)(down + 30);
    g_enemy_select = enemy + 6;
    g_enemy_attack = enemy + 4 + (signed char)enemy[3];
    g_unit_list = *(DWORD **)(desel + 2);
    g_play_sound = (void *)snd;

    BYTE *call = ground + 14;
    g_ground_orig = rel32(call + 1);
    DWORD old;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int *)(call + 1) = (int)((BYTE *)ground_stub - (call + 5));
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);

    if (!rr_write_jmp(down, (void *)down_stub, 12) || !rr_write_jmp(enemy, (void *)enemy_stub, 6)) return 0;
    rr_log("input: right click commands robots, left click on the ground deselects (%p %p %p)", down, enemy, call);
    return 1;
}

static int install_pan(void)
{
#define ARROW(vk) "\xb8" vk "\x00\x00\x00\x89\x04\x24\xe8????\x85\xc0\x74\x0c\xdb\x05????\xd9\x1d????"
#define ARROW_M   "xxxxxxxxx????xxxxxx????xx????"
    BYTE *p = find_masked(ARROW("\x25") ARROW("\x27") ARROW("\x26") ARROW("\x28") "\xd9\x05????\xd9\x05",
                          ARROW_M ARROW_M ARROW_M ARROW_M "xx????xx");
    if (!p) { rr_log("input: camera scroll code not found, panning off"); return 0; }
    for (int i = 0; i < 4; i++) g_vel[i] = *(float **)(p + i * 29 + 25);
    BYTE *hook = p + 4 * 29;
    g_vel_min = *(float **)(hook + 8);
    g_arrows_resume = hook + 6;
    if (*(float **)(hook + 2) != g_vel[0] || !rr_write_jmp(hook, (void *)arrows_stub, 6)) {
        rr_log("input: camera scroll code differs, panning off");
        return 0;
    }
    rr_log("input: right-drag pans the camera (%p)", hook);
    return 1;
}

void input_install(void)
{
    g_right_cmds = GetPrivateProfileIntA("input", "right_click_commands", 1, g_ini_path);
    g_pan = GetPrivateProfileIntA("input", "right_drag_pan", 1, g_ini_path);
    g_wasd = GetPrivateProfileIntA("input", "wasd_camera", 1, g_ini_path);
    g_pan_grab = GetPrivateProfileIntA("input", "pan_grab", 1, g_ini_path);
    char buf[32];
    GetPrivateProfileStringA("input", "pan_speed", "1.0", buf, sizeof buf, g_ini_path);
    g_pan_speed = (float)atof(buf);
    if (!g_right_cmds && !g_wasd) return;

    if (!rr_watch_modes()) rr_log("input: mode switch not found");
    if (g_right_cmds && !install_clicks()) g_right_cmds = 0;
    if (!g_right_cmds) g_pan = 0;          /* panning lives on the right button's click */
    if (g_pan && !install_pan()) g_pan = 0;
    if (!g_pan && !g_wasd) return;

    HMODULE exe = GetModuleHandleA(NULL);
    prev_PeekMessageA = rr_hook_iat(exe, "USER32.dll", "PeekMessageA", (void *)in_PeekMessageA);
    prev_GetMessageA = rr_hook_iat(exe, "USER32.dll", "GetMessageA", (void *)in_GetMessageA);
    if (g_wasd) rr_log("input: WASD scrolls the camera during missions");
}
