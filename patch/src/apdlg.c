/* Archipelago menus in the briefing dialogs.

   The briefing's characters talk through scripts (nls.pak: a-proffs.dlg, a-robo.dlg, common.dlg, ...):
       :label            a block; TALKER / YOU say something, ">>label" lines are the choices
       SWITCH            a block without text: SETVARIABLE name value, IF name = value THEN label, GOTO label
       QUESTION text     the question above a YOU block's choices
   The robot Zuom IV (rightmost portrait) shows common.dlg's do_menu. With Archipelago on, the patch hands the game
   its own common.dlg: do_menu gets "Choose a mission" in front, leading to a side and mission list.

   Script and patch talk through the dialog variables (a table of { char name[40]; int value; }):
       AP_ON, AP_BOTH, AP_SIDE           written by the patch: connected, both sides played, side of this campaign
       AP_OPEN_A_n, AP_OPEN_B_n          written by the patch: mission n of Red Star / Terraflux is unlocked
       AP_PICK                           written by the script: 1-16 Red Star, 101-116 Terraflux
   After a pick the briefing is left and entered again, so it shows the chosen mission. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

int ap_side_mode(void);                    /* -1 = no slot, 0/1 = one side, 2 = both */
int ap_mission_open(int side, int n);
void ap_choose_mission(int side, int n);
int ap_campaign_side(void);
int ap_mission_in_progress(void);
void ap_abandon_mission(void);

typedef struct { char name[40]; int value; } DlgVar;
static DlgVar *g_vars;
static int *g_nvars;
#define MAX_VARS 100
static int *g_choice_count, *g_choice_greyed, *g_redraw;
static char *g_choice_labels;

/* ---------------------------------------------------------------- the script */
static const char *k_script =
    "\n"
    ":ap_back\n"
    "SWITCH\n"
    "IF AP_INGAME = 1 THEN do_menu_in_game\n"
    "GOTO do_menu\n"
    "\n"
    ":ap_missions\n"
    "SWITCH\n"
    "IF AP_ON = 1 THEN ap_missions_on\n"
    "GOTO ap_missions_off\n"
    "\n"
    ":ap_missions_off\n"
    "TALKER\n"
    "  >>ap_back\n"
    "    Archipelago isn't c-c-c-connected,\n"
    "    master. Start the RoboRumble client\n"
    "    and load a campaign first.\n"
    "\n"
    ":ap_missions_on\n"
    "SWITCH\n"
    "IF AP_BOTH = 1 THEN ap_sides\n"
    "IF AP_SIDE = 1 THEN ap_b1\n"
    "GOTO ap_a1\n"
    "\n"
    ":ap_sides\n"
    "YOU\n"
    "  >>ap_side_a\n"
    "    Red Star missions.\n"
    "  >>ap_side_b\n"
    "    Terraflux missions.\n"
    "  >>ap_back\n"
    "    ...never mind.\n"
    "QUESTION Which campaign?\n"
    "\n"
    ":ap_side_a\n"
    "SWITCH\n"
    "GOTO ap_a1\n"
    "\n"
    ":ap_side_b\n"
    "SWITCH\n"
    "GOTO ap_b1\n"
    "\n"
    ":ap_locked\n"
    "TALKER\n"
    "  >>ap_back\n"
    "    That mission is still l-l-l-locked,\n"
    "    master.\n"
    "\n"
    ":ap_picked\n"
    "TALKER\n"
    "  >>ap_back\n"
    "    Very well, master. The b-b-b-briefing\n"
    "    for that mission is on its way.\n";

static char *append(char *buf, size_t *len, size_t *cap, const char *text)
{
    size_t n = strlen(text);
    if (*len + n + 1 > *cap) {
        *cap = (*len + n + 1) * 2;
        buf = realloc(buf, *cap);
    }
    memcpy(buf + *len, text, n + 1);
    *len += n;
    return buf;
}

/* the mission lists: two pages of eight per side */
static char *mission_lists(char *buf, size_t *len, size_t *cap)
{
    char line[256];
    for (int side = 0; side < 2; side++) {
        char s = side ? 'b' : 'a';
        for (int page = 0; page < 2; page++) {
            snprintf(line, sizeof line, "\n:ap_%c%d\nYOU\n", s, page + 1);
            buf = append(buf, len, cap, line);
            for (int n = page * 8 + 1; n <= page * 8 + 8; n++) {
                snprintf(line, sizeof line, "  >>ap_pick_%c_%d\n    Mission %d%s\n", s, n, n, n == 16 ? " (final)" : "");
                buf = append(buf, len, cap, line);
            }
            snprintf(line, sizeof line, "  >>ap_%c%d\n    %s\n  >>ap_back\n    ...never mind.\n"
                     "QUESTION Which %s mission?\n", s, 2 - page, page ? "Missions 1 to 8..." : "Missions 9 to 16...",
                     side ? "Terraflux" : "Red Star");
            buf = append(buf, len, cap, line);
        }
        for (int n = 1; n <= 16; n++) {
            snprintf(line, sizeof line,
                     "\n:ap_pick_%c_%d\nSWITCH\nIF AP_OPEN_%c_%d = 1 THEN ap_go_%c_%d\nGOTO ap_locked\n"
                     "\n:ap_go_%c_%d\nSWITCH\nSETVARIABLE AP_PICK %d\nGOTO ap_picked\n",
                     s, n, s - 32, n, s, n, s, n, side * 100 + n);
            buf = append(buf, len, cap, line);
        }
    }
    return buf;
}

/* "\n" -> the file's own line ends */
static char *with_line_ends(const char *text, int crlf, size_t *outlen)
{
    size_t n = strlen(text), extra = 0;
    for (size_t i = 0; i < n; i++) extra += text[i] == '\n';
    char *out = malloc(n + (crlf ? extra : 0) + 1), *o = out;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n' && crlf) *o++ = '\r';
        *o++ = text[i];
    }
    *o = 0;
    *outlen = o - out;
    return out;
}

void apdlg_generate(void)
{
    if (!GetPrivateProfileIntA("ap", "enabled", 0, g_ini_path)) return;
    DWORD size = 0;
    char *src = rr_pak_read("pak\\nls.pak", "common.dlg", &size);
    if (!src) { rr_log("ap: common.dlg not found in pak\\nls.pak, no mission menu"); return; }
    int crlf = strstr(src, "\r\n") != NULL;
    const char *nl = crlf ? "\r\n" : "\n";

    /* new first choices of the robot's menus: between briefings (do_menu) and during a mission (do_menu_in_game) */
    static const struct { const char *label, *choices; } k_menus[] = {
        { "do_menu", "  >>ap_missions\n    Choose a mission\n  >>save\n    Save the game\n" },
        { "do_menu_in_game", "  >>ap_missions\n    Choose a mission\n" },
    };
    size_t cap = size + 16384, len = 0, clen;
    char *out = malloc(cap);
    out[0] = 0;
    const char *rest = src;
    int added = 0;
    for (int m = 0; m < 2; m++) {
        char marker[48];
        snprintf(marker, sizeof marker, ":%s%sYOU%s", k_menus[m].label, nl, nl);
        const char *at = strstr(rest, marker);
        if (!at) { rr_log("ap: %s not found in common.dlg", k_menus[m].label); continue; }
        size_t head = (at - rest) + strlen(marker);
        char *part = malloc(head + 1);
        memcpy(part, rest, head);
        part[head] = 0;
        out = append(out, &len, &cap, part);
        free(part);
        char *c = with_line_ends(k_menus[m].choices, crlf, &clen);
        out = append(out, &len, &cap, c);
        free(c);
        rest += head;
        added++;
    }
    if (!added) { free(out); free(src); return; }
    out = append(out, &len, &cap, rest);

    char *script = NULL;
    size_t slen = 0, scap = 0;
    script = append(script, &slen, &scap, k_script);
    script = mission_lists(script, &slen, &scap);
    char *s = with_line_ends(script, crlf, &clen);
    out = append(out, &len, &cap, s);
    free(s);
    free(script);

    if (rr_vfs_generate("common.dlg", out, (DWORD)len)) rr_log("ap: mission menu added to common.dlg");
    free(out);
    free(src);
}

/* ---------------------------------------------------------------- variables */
static void set_var(const char *name, int value)
{
    for (int i = 0; i < *g_nvars; i++)
        if (!strcmp(g_vars[i].name, name)) { g_vars[i].value = value; return; }
    if (*g_nvars >= MAX_VARS) return;
    DlgVar *v = &g_vars[(*g_nvars)++];
    memset(v, 0, sizeof *v);
    lstrcpynA(v->name, name, sizeof v->name);
    v->value = value;
}

static int get_var(const char *name, int dflt)
{
    for (int i = 0; i < *g_nvars; i++)
        if (!strcmp(g_vars[i].name, name)) return g_vars[i].value;
    return dflt;
}

/* the briefing ends its loop when this is set; it returns g_reenter_mode: BRIEFING starts it over, LOADLEVEL
   (after a side switch) goes the way a loaded save goes */
BYTE g_reenter;
DWORD g_reenter_mode = RR_MODE_BRIEFING;
void *g_reenter_resume, *g_reenter_exit;
BYTE *g_reenter_flag;
__asm__(
    ".text\n"
    ".globl _reenter_stub\n"
    "_reenter_stub:\n"
    "    cmpb $0, _g_reenter\n"
    "    je 1f\n"
    "    movb $0, _g_reenter\n"
    "    movl _g_reenter_mode, %eax\n"
    "    jmp *_g_reenter_exit\n"
    "1:  movl _g_reenter_flag, %eax\n"
    "    movb (%eax), %al\n"
    "    jmp *_g_reenter_resume\n");
void reenter_stub(void);

static int locate(void)
{
    static int done = -1;
    if (done >= 0) return done;
    done = 0;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *code = NULL;
    /* mov esi, "CURRENT_LEVEL"; lea ebp,[eax+ebx*2]; lea edi,[ebp*4 + table]  (briefing start) */
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !code; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 32;
        for (; p < end; p++) {
            if (p[0] != 0xBE || p[5] != 0x8D || p[6] != 0x2C || p[7] != 0x58 || p[8] != 0x8D || p[9] != 0x3C ||
                p[10] != 0xAD) continue;
            const char *str = *(const char **)(p + 1);
            if (!IsBadReadPtr(str, 14) && !memcmp(str, "CURRENT_LEVEL", 14)) { code = p; break; }
        }
    }
    if (!code) { rr_log("ap: dialog variables not found"); return 0; }
    g_vars = *(DlgVar **)(code + 11);
    for (BYTE *q = code; q > code - 0x40; q--)   /* mov ebx, [count] before it */
        if (q[0] == 0x8B && q[1] == 0x1D) { g_nvars = *(int **)(q + 2); break; }
    if (!g_nvars) { rr_log("ap: dialog variable count not found"); return 0; }

    /* mov al,[flag]; test al,al; je; mov eax,[mode]; cmp eax,140h; jne; mov eax,[next]; test eax,eax; jne exit */
    BYTE *r = NULL;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !r; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 40;
        for (; p < end; p++)
            if (p[0] == 0xA0 && p[5] == 0x84 && p[6] == 0xC0 && p[7] == 0x74 && p[8] == 0x15 && p[9] == 0xA1 &&
                p[14] == 0x3D && p[15] == 0x40 && p[16] == 0x01 && p[17] == 0 && p[18] == 0 && p[19] == 0x75 &&
                p[20] == 0x09 && p[21] == 0xA1 && p[26] == 0x85 && p[27] == 0xC0 && p[28] == 0x75 && p[29] == 0x11) {
                r = p;
                break;
            }
    }
    if (r) {
        g_reenter_flag = *(BYTE **)(r + 1);
        g_reenter_resume = r + 5;
        g_reenter_exit = r + 30 + 0x11;
        rr_write_jmp(r, (void *)reenter_stub, 5);
    } else rr_log("ap: briefing loop end not found, a mission pick shows from the next briefing on");

    /* the choice list on screen, from the code that lists the engineer's boosters:
       mov eax,[count]; mov edx,"buy_booster_%d"; ...; lea eax,[eax*8 + labels]; ...; mov [eax*4 + greyed],edx */
    const char *bb = NULL;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !bb; i++, sec++) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 16;
        for (; p < end; p++) if (!memcmp(p, "buy_booster_%d", 15)) { bb = (const char *)p; break; }
    }
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && bb && !g_choice_greyed; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 64;
        for (; p < end; p++) {
            /* a1 count / ba fmt / 89 54 24 04 / 89 5c 24 08 / 8d 04 80 / 8d 04 c5 labels */
            if (p[0] != 0xA1 || p[5] != 0xBA || *(const char **)(p + 6) != bb || p[21] != 0x8D || p[22] != 0x04 ||
                p[23] != 0xC5) continue;
            for (BYTE *q = p + 28; q < p + 64; q++)
                if (q[0] == 0x89 && q[1] == 0x14 && q[2] == 0x85) {
                    g_choice_count = *(int **)(p + 1);
                    g_choice_labels = *(char **)(p + 24);
                    g_choice_greyed = *(int **)(q + 3);
                    break;
                }
            if (g_choice_greyed) break;
        }
    }
    /* briefing start: ...; mov [redraw counter],ecx (3: the background is drawn again on every buffer) */
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_redraw; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 40;
        for (; p < end; p++)
            if (p[0] == 0x88 && p[1] == 0x15 && p[6] == 0x88 && p[7] == 0x15 && p[12] == 0x8B && p[13] == 0x15 &&
                p[18] == 0x89 && p[19] == 0x0D && p[24] == 0x8B && p[25] == 0x0D && p[30] == 0xA3) {
                g_redraw = *(int **)(p + 20);
                break;
            }
    }
    rr_log("ap: dialog variables at %p (count %p), briefing restart %p, choices %p/%p/%p, redraw %p",
           g_vars, g_nvars, r, g_choice_count, g_choice_labels, g_choice_greyed, g_redraw);
    done = 1;
    return 1;
}

/* Locked missions in the mission lists are greyed out, like goods the engineer can't sell. */
static void grey_locked(int side_mode)
{
    if (!g_choice_count || !g_choice_labels || !g_choice_greyed) return;
    int n = *g_choice_count;
    if (n < 0) n = -n;
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) {
        const char *label = g_choice_labels + i * 0x28;
        if (strncmp(label, "ap_pick_", 8) || (label[8] != 'a' && label[8] != 'b') || label[9] != '_') continue;
        int s = label[8] == 'b', m = atoi(label + 10);
        g_choice_greyed[i] = side_mode < 0 || !ap_mission_open(s, m);
    }
}

/* game thread, every few frames */
void apdlg_tick(void)
{
    DWORD mode = g_gui_mode;
    if (mode != RR_MODE_BRIEFING && mode != RR_MODE_MENU) return;
    if (!locate()) return;
    int side_mode = ap_side_mode();
    set_var("AP_ON", side_mode >= 0);
    int ingame = ap_mission_in_progress();
    set_var("AP_INGAME", ingame);
    grey_locked(side_mode);

    /* money changed behind the game's back (Shop Money, dev window): the background with the old amount is still on
       one of the buffers, so it is drawn again */
    static int last_money = -1;
    const RRGame *Gm = rr_game();
    int money = *(int *)(Gm->state + 8);
    if (money != last_money) {
        if (last_money >= 0 && g_redraw && *g_redraw < 3) *g_redraw = 3;
        last_money = money;
    }
    if (side_mode < 0) return;
    int side = ap_campaign_side();
    set_var("AP_BOTH", side_mode == 2);
    set_var("AP_SIDE", side);
    char name[40];
    for (int s = 0; s < 2; s++)
        for (int n = 1; n <= 16; n++) {
            snprintf(name, sizeof name, "AP_OPEN_%c_%d", s ? 'B' : 'A', n);
            set_var(name, ap_mission_open(s, n));
        }
    int pick = get_var("AP_PICK", 0);
    if (pick) {
        set_var("AP_PICK", 0);
        int ps = pick > 100 ? 1 : 0, pn = pick % 100;
        rr_log("ap: mission %d of side %d picked in the briefing", pn, ps);
        ap_choose_mission(ps, pn);
        if (mode == RR_MODE_BRIEFING) {
            g_reenter_mode = RR_MODE_BRIEFING;
            if (ingame) {
                /* from the menu during a mission: that mission is given up, the picked one loaded */
                const RRGame *G = rr_game();
                ap_abandon_mission();
                *(int *)G->state = ps;
                *(int *)(G->state + 4) = pn - 1;
                g_reenter_mode = RR_MODE_LOADLEVEL;
            } else if (ps != side) {
                /* the other campaign: side and mission in the state block, then the level loading a loaded save
                   goes through (a save holds nothing more about its side) */
                const RRGame *G = rr_game();
                *(int *)G->state = ps;
                *(int *)(G->state + 4) = pn - 1;
                g_reenter_mode = RR_MODE_LOADLEVEL;
                rr_log("ap: campaign side switched to %d", ps);
            }
            g_reenter = 1;
        }
    }
}
