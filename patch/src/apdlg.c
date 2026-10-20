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
int ap_take_client_pick(void);
int ap_campaign_side(void);
int ap_mission_in_progress(void);
void ap_abandon_mission(void);
void ap_set_mission(int side, int n);
int ap_goal_done(void);
void ap_epilogue_watched(void);
int ap_artifacts_missing(void);
void ap_shop_label(int slot, char *out, int size);
int ap_shop_slot_state(int slot);
void ap_shop_opened(void);
static char *g_choice_text;   /* the choices' texts, 0x8c each */
static void (__cdecl *g_outro)(void);   /* the game's ending: fade out, outro2.avi */
static int g_epilogue;
static void epilogue_mode(DWORD mode)
{
    if (mode == 15 && g_epilogue) {
        g_epilogue = 0;
        if (g_outro) g_outro();
    }
}
static void (__cdecl *g_door_close)(void);   /* the briefing's doors close (with their sound) */
static DWORD *g_next_mode;                   /* the briefing leaves for this mode once the doors are shut */
static BYTE *g_door_flag;

typedef struct { char name[40]; int value; } DlgVar;
static DlgVar *g_vars;
static int *g_nvars;
#define MAX_VARS 100
static int *g_choice_count, *g_choice_greyed, *g_redraw;
static char *g_choice_labels;

/* ---------------------------------------------------------------- the script */
static const char *k_script =
    "\n"
    ":ap_do_menu\n"
    "SWITCH\n"
    "IF AP_GOAL = 1 THEN ap_do_menu_goal\n"
    "IF AP_ARTGOAL = 1 THEN ap_do_menu_goal\n"
    "GOTO ap_do_menu_plain\n"
    "\n"
    ":ap_do_menu_goal\n"
    "YOU\n"
    "  >>ap_epilogue\n"
    "    Watch the epilogue\n"
    "  >>ap_missions\n"
    "    Choose a mission\n"
    "  >>save\n"
    "    Save the game\n"
    "  >>load\n"
    "    Load a game\n"
    "  >>options\n"
    "    Set some options\n"
    "  >>areyousure_quit\n"
    "    Quit the game\n"
    "SETVARIABLE ROBOT_WELCOME 1\n"
    "\n"
    ":ap_epilogue\n"
    "TALKER\n"
    "  >>ap_epilogue_go\n"
    "    Master, the Alien Artifacts are all\n"
    "    ours. Whatever the Aliens planned\n"
    "    with them, it is over now. The war\n"
    "    is w-w-w-w-won, master.\n"
    "\n"
    ":ap_epilogue_go\n"
    "SWITCH\n"
    "SETVARIABLE AP_PICK 997\n"
    "GOTO close_spy\n"
    "\n"
    ":ap_do_menu_plain\n"
    "YOU\n"
    "  >>ap_missions\n"
    "    Choose a mission\n"
    "  >>save\n"
    "    Save the game\n"
    "  >>load\n"
    "    Load a game\n"
    "  >>options\n"
    "    Set some options\n"
    "  >>areyousure_quit\n"
    "    Quit the game\n"
    "SETVARIABLE ROBOT_WELCOME 1\n"
    "\n"
    ":ap_do_menu_in_game\n"
    "YOU\n"
    "  >>ap_missions\n"
    "    Choose a mission\n"
    "  >>ap_abandon\n"
    "    Abandon mission\n"
    "  >>return_to_game\n"
    "    Return to mission\n"
    "  >>restart_game\n"
    "    Restart mission\n"
    "  >>load\n"
    "    Load a game\n"
    "  >>options\n"
    "    Set some options\n"
    "  >>areyousure_quit\n"
    "    Quit the game\n"
    "SETVARIABLE ROBOT_WELCOME 1\n"
    "\n"
    ":ap_back\n"
    "SWITCH\n"
    "IF AP_INGAME = 1 THEN do_menu_in_game\n"
    "GOTO do_menu\n"
    "\n"
    ":ap_abandon\n"
    "SWITCH\n"
    "SETVARIABLE AP_PICK 999\n"
    "GOTO close_spy\n"   /* the game's own "close the dialog" action */
    "\n"
    ":ap_abandoned\n"
    "TALKER\n"
    "  >>do_menu\n"
    "    Mission abandoned, master. B-b-b-back\n"
    "    to the briefing.\n"
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
                     "\n:ap_go_%c_%d\nSWITCH\nSETVARIABLE AP_PICK %d\nGOTO close_spy\n",
                     s, n, s - 32, n, s, n, s, n, side * 100 + n);
            buf = append(buf, len, cap, line);
        }
    }
    return buf;
}

/* ---------------------------------------------------------------- the engineer's shop
   The engineer's script (a-proffs.dlg / b-proffs.dlg) gets, with Archipelago on:
   - AP_SHOP set when his menu opens: the client hints what the shop's goods are
   - the price question naming what you get ("That would be |AP_ITEM. I want |PRICE..."), also for the secret
     inventions, and a short line instead of the invention's story after buying one
   |AP_ITEM is filled in by the hooked text token function below. */
static const char *k_proffs_script =
    "\n"
    ":P_ap_price\n"
    "TALKER\n"
    "  >>P_price_answer\n"
    "    That would be |AP_ITEM.\n"
    "    I want |PRICE for it. Agreed?\n"
    "\n"
    ":P_ap_secret\n"
    "TALKER\n"
    "  >>P_acquired_secret\n"
    "    My secret invention is...\n"
    "    |AP_ITEM!\n";

/* the text after the first `marker` gets `text` inserted; 0 if the marker isn't there */
static int insert_after(char **buf, const char *marker, const char *text)
{
    char *at = strstr(*buf, marker);
    if (!at) return 0;
    size_t head = at - *buf + strlen(marker), tl = strlen(text), rest = strlen(*buf) - head;
    char *out = malloc(head + tl + rest + 1);
    memcpy(out, *buf, head);
    memcpy(out + head, text, tl);
    memcpy(out + head + tl, *buf + head, rest + 1);
    free(*buf);
    *buf = out;
    return 1;
}

static char *with_line_ends(const char *text, int crlf, size_t *outlen);

static void generate_proffs(const char *name)
{
    DWORD size = 0;
    char *src = rr_pak_read("pak\\nls.pak", name, &size);
    if (!src) { rr_log("ap: %s not found, shop texts unchanged", name); return; }
    int crlf = strstr(src, "\r\n") != NULL;
    size_t n;
    static const char *const edits[][2] = {
        { ":P_inventions\nSWITCH\n", "SETVARIABLE AP_SHOP 1\n" },
        { ":P_price\nSWITCH\n", "IF AP_ON = 1 THEN P_ap_price\n" },
        { ":P_price_accepted_surprise_1\n",
          "SWITCH\nIF AP_ON = 1 THEN P_ap_secret\nGOTO P_price_accepted_surprise_1v\n\n:P_price_accepted_surprise_1v\n" },
        { ":P_price_accepted_surprise_2\n",
          "SWITCH\nIF AP_ON = 1 THEN P_ap_secret\nGOTO P_price_accepted_surprise_2v\n\n:P_price_accepted_surprise_2v\n" },
        { ":P_price_accepted_surprise_3\n",
          "SWITCH\nIF AP_ON = 1 THEN P_ap_secret\nGOTO P_price_accepted_surprise_3v\n\n:P_price_accepted_surprise_3v\n" },
    };
    int done = 0;
    for (size_t i = 0; i < sizeof edits / sizeof *edits; i++) {
        char *m = with_line_ends(edits[i][0], crlf, &n), *t = with_line_ends(edits[i][1], crlf, &n);
        done += insert_after(&src, m, t);
        free(m);
        free(t);
    }
    char *tail = with_line_ends(k_proffs_script, crlf, &n);
    size_t len = strlen(src);
    src = realloc(src, len + n + 1);
    memcpy(src + len, tail, n + 1);
    free(tail);
    if (rr_vfs_generate(name, src, (DWORD)strlen(src))) rr_log("ap: shop texts in %s (%d of 5 places)", name, done);
    free(src);
}

/* the names the shop lists show: ours (what buying sends) or the game's own */
static char **g_booster_names_orig, **g_upgrade_names_orig;
static char *g_booster_names[9], *g_upgrade_names[8];
static char g_list_text[17][48];

/* AP label for the list, cut off when long */
static void list_label(int slot, char *out, int size)
{
    char text[64];
    ap_shop_label(slot, text, sizeof text);
    if ((int)strlen(text) >= size) {
        lstrcpynA(out, text, size - 3);
        strcat(out, "...");
    } else lstrcpyA(out, text);
}

/* before each list is built (briefing ticks): ours where the client said what buying sends */
static void shop_names(void)
{
    for (int i = 0; i < 17; i++) {
        char **orig = i < 9 ? g_booster_names_orig : g_upgrade_names_orig;
        char **ours = i < 9 ? g_booster_names : g_upgrade_names;
        int k = i < 9 ? i : i - 9;
        if (!orig) continue;
        list_label(i, g_list_text[i], 40);
        ours[k] = g_list_text[i][0] ? g_list_text[i] : orig[k];
    }
}

/* What is being bought (the shop's purchase state): price, then +0x0C upgrade index, +0x10 booster index,
   +0x14 invention index, +0x18 kind (0 booster, 1 invention, 2 upgrade) */
static BYTE *g_purchase;

/* The game fills "|NAME" in dialog texts through one function (eax = output, edx = NAME). AP_ITEM is ours. */
void *g_token_resume;
int __cdecl ap_token(char *out, const char *name)
{
    if (strcmp(name, "AP_ITEM") || !g_purchase) return 0;
    int kind = *(int *)(g_purchase + 0x18), slot = -1;
    if (kind == 0) slot = *(int *)(g_purchase + 0x10);
    else if (kind == 1) slot = 19 + *(int *)(g_purchase + 0x14);
    else if (kind == 2) slot = 9 + *(int *)(g_purchase + 0x0C);
    char text[64];
    ap_shop_label(slot, text, sizeof text);
    lstrcpynA(out, text[0] ? text : "something new", 64);
    return 1;
}
__asm__(
    ".text\n"
    ".globl _token_stub\n"
    "_token_stub:\n"
    "    pushal\n"
    "    pushl %edx\n"
    "    pushl %eax\n"
    "    call _ap_token\n"
    "    addl $8, %esp\n"
    "    testl %eax, %eax\n"
    "    popal\n"
    "    jnz 1f\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    subl $0x134, %esp\n"
    "    jmp *_g_token_resume\n"
    "1:  ret\n");
void token_stub(void);

static void shop_install(void)
{
    static const BYTE token[] = { 0x57, 0x56, 0x55, 0x53, 0x81, 0xEC, 0x34, 0x01, 0x00, 0x00, 0x8B, 0xDA, 0x8B, 0xEC,
                                  0x83, 0xC5, 0x7C, 0x89, 0x84, 0x24, 0x2C, 0x01, 0x00, 0x00, 0xBE };
    BYTE *fn = rr_find_unique(token, sizeof token);
    if (fn) {
        g_token_resume = fn + 10;
        if (!rr_write_jmp(fn, (void *)token_stub, 10)) fn = NULL;
    }
    /* booster purchase: xor edx,edx; mov [shown price],eax; mov [price],eax; mov eax,[..]; mov [kind],edx;
       mov [ebp-2Ch],eax; mov [booster],esi */
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_purchase; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 40;
        for (; p < end; p++) {
            if (p[0] != 0x33 || p[1] != 0xD2 || p[2] != 0xA3 || p[7] != 0xA3 || p[12] != 0xA1 || p[17] != 0x89 ||
                p[18] != 0x15 || p[23] != 0x89 || p[24] != 0x45 || p[25] != 0xD4 || p[26] != 0x89 || p[27] != 0x35)
                continue;
            DWORD price = *(DWORD *)(p + 8);
            if (*(DWORD *)(p + 19) == price + 0x18 && *(DWORD *)(p + 28) == price + 0x10) {
                g_purchase = (BYTE *)price;
                break;
            }
        }
    }
    /* the booster and base upgrade lists take their names from two pointer tables (mov edi,[ebx*4+names]):
       pointed at our own tables, the lists are built (and their box sized) with what buying sends */
    int lists = 0;
    for (int i = 0, n = 0; i < nt->FileHeader.NumberOfSections && n < 2; i++) {
        IMAGE_SECTION_HEADER *sc = IMAGE_FIRST_SECTION(nt) + i;
        if (!(sc->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sc->VirtualAddress, *end = p + sc->Misc.VirtualSize - 16;
        for (; p < end && n < 2; p++) {
            if (p[0] != 0xA1 || p[5] != 0x8B || p[6] != 0x3C || p[7] != 0x9D || p[12] != 0x8D || p[13] != 0x04 ||
                p[14] != 0x80 || p[15] != 0x8B || p[16] != 0xD0)
                continue;
            char ***table = n == 0 ? &g_booster_names_orig : &g_upgrade_names_orig;
            *table = *(char ***)(p + 8);
            DWORD ours = n == 0 ? (DWORD)g_booster_names : (DWORD)g_upgrade_names, old;
            VirtualProtect(p + 8, 4, PAGE_EXECUTE_READWRITE, &old);
            *(DWORD *)(p + 8) = ours;
            VirtualProtect(p + 8, 4, old, &old);
            n++;
            lists = n;
        }
    }
    rr_log("ap: shop texts: token function %p, purchase %p, lists %d", (void *)fn, (void *)g_purchase, lists);
}

/* Shop lists on screen: each good shows what buying it sends, greyed once bought (Camouflage too is sold at any
   time; the game's own rule needs exactly one Maximum booster and mission 14). */
static void shop_lists(void)
{
    if (!g_choice_count || !g_choice_labels || !g_choice_greyed) return;
    int n = *g_choice_count;
    if (n < 0) n = -n;
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) {
        const char *label = g_choice_labels + i * 0x28;
        int slot;
        if (!strncmp(label, "buy_booster_", 12)) slot = atoi(label + 12);
        else if (!strncmp(label, "buy_base_upgrade_", 17)) slot = 9 + atoi(label + 17);
        else continue;
        int state = ap_shop_slot_state(slot);
        if (state < 0) continue;
        g_choice_greyed[i] = state;
    }
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
        { "do_menu_in_game", "  >>ap_missions\n    Choose a mission\n  >>ap_abandon\n    Abandon mission\n" },
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
        size_t head = at - rest;
        char *part = malloc(head + 1);
        memcpy(part, rest, head);
        part[head] = 0;
        out = append(out, &len, &cap, part);
        free(part);
        /* while connected the robot shows the Archipelago version of the menu (ap_do_menu...), which has no new
           campaign and no multiplayer: the campaign belongs to the slot */
        char sw[256];
        snprintf(sw, sizeof sw, ":%s\nSWITCH\nIF AP_ON = 1 THEN ap_%s\nGOTO %s_full\n\n:%s_full\nYOU\n",
                 k_menus[m].label, k_menus[m].label, k_menus[m].label, k_menus[m].label);
        char *c = with_line_ends(sw, crlf, &clen);
        out = append(out, &len, &cap, c);
        free(c);
        rest = at;
        head = strlen(marker);
        c = with_line_ends(k_menus[m].choices, crlf, &clen);
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
    generate_proffs("a-proffs.dlg");
    generate_proffs("b-proffs.dlg");
    shop_install();
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
        g_door_flag = *(BYTE **)(r + 1);
        g_next_mode = *(DWORD **)(r + 22);
    }
    /* mov al,[doors]; test al,al; jne +0f; call door_close; mov dword [next mode],0Dh  (Load a game) */
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_door_close && g_door_flag; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 24;
        for (; p < end; p++)
            if (p[0] == 0xA0 && *(BYTE **)(p + 1) == g_door_flag && p[5] == 0x84 && p[6] == 0xC0 && p[7] == 0x75 &&
                p[8] == 0x0F && p[9] == 0xE8 && p[14] == 0xC7 && p[15] == 0x05 && *(DWORD **)(p + 16) == g_next_mode &&
                p[20] == 0x0D) {
                g_door_close = (void *)(p + 14 + *(int *)(p + 10));
                break;
            }
    }
    if (!g_door_close) rr_log("ap: briefing doors not found, picking a mission won't work");

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
                    /* before it, the text copy: lea esi,[eax*4 + texts]; mov dl,[edi]; ... jne */
                    if (p[-17] == 0x8D && p[-16] == 0x34 && p[-15] == 0x85) g_choice_text = *(char **)(p - 14);
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
    /* the ending: the function that uses "pak2\outro2.avi", found back from that reference to its prologue
       (push edi; push esi; push ebp; push ebx; sub esp,..) */
    {
        const char *outro = NULL;
        sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !outro; i++, sec++) {
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 16;
            for (; p < end; p++) if (!memcmp(p, "outro2.avi", 10) && p[-1] == '\\') { outro = (const char *)p - 5; break; }
        }
        sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && outro && !g_outro; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 4;
            for (; p < end; p++) {
                if (*(const char **)p != outro) continue;
                for (BYTE *q = p; q > p - 0x400; q--)
                    if (q[0] == 0x57 && q[1] == 0x56 && q[2] == 0x55 && q[3] == 0x53 && q[4] == 0x83 && q[5] == 0xEC &&
                        (q[-1] == 0x00 || q[-1] == 0xC3 || q[-1] == 0x90 || q[-1] == 0xCC)) {
                        g_outro = (void *)q;
                        break;
                    }
                break;
            }
        }
        if (!g_outro) rr_log("ap: ending not found, the epilogue shows only the credits");
    }
    rr_on_mode(epilogue_mode);
    rr_log("ap: dialog variables at %p (count %p), doors %p, choices %p/%p/%p, redraw %p",
           g_vars, g_nvars, (void *)g_door_close, g_choice_count, g_choice_labels, g_choice_greyed, g_redraw);
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
        if (!strcmp(label, "ap_epilogue")) {
            /* the artifact goal: greyed with the count until it is reached */
            int missing = ap_artifacts_missing();
            int done = ap_goal_done() || missing == 0;
            g_choice_greyed[i] = !done;
            if (g_choice_text) {
                char *t = g_choice_text + i * 0x8c;
                if (done) lstrcpynA(t, "Watch the epilogue", 0x8c);
                else snprintf(t, 0x8c, "Epilogue (%d more Alien Artifact%s)", missing, missing == 1 ? "" : "s");
            }
            continue;
        }
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
    set_var("AP_GOAL", ap_goal_done());
    set_var("AP_ARTGOAL", ap_artifacts_missing() >= 0);
    grey_locked(side_mode);
    if (side_mode >= 0) shop_lists();
    shop_names();
    if (get_var("AP_SHOP", 0)) {
        set_var("AP_SHOP", 0);
        if (side_mode >= 0) ap_shop_opened();
    }

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
    /* the client's /mission: in a briefing it loads now, like a pick here; elsewhere it already waits for the next
       level load */
    int client_pick = ap_take_client_pick();
    if (!pick && client_pick && mode == RR_MODE_BRIEFING) pick = client_pick;
    if (pick) {
        set_var("AP_PICK", 0);
        if (pick == 997) {   /* the epilogue: the game's ending (outro2), then the credits */
            rr_log("ap: epilogue");
            ap_epilogue_watched();
            g_epilogue = 1;   /* the outro plays when the credits screen starts */
            if (g_door_close && g_next_mode && g_door_flag) {
                if (!*g_door_flag) g_door_close();
                *g_next_mode = 15;   /* CREDITS */
            }
            return;
        }
        int ps = pick > 100 ? 1 : 0, pn = pick % 100;
        if (pick == 999) {   /* Abandon mission: the briefing of this mission again */
            ps = side;
            pn = *(int *)(rr_game()->state + 4) + 1;
        }
        rr_log("ap: mission %d of side %d picked in the briefing", pn, ps);
        if (mode == RR_MODE_BRIEFING) {
            /* The mission (and side) to load, then out through the doors into the level loading, like a loaded
               save: the level's map, funds and parts are set up there. A mission in progress is given up. */
            if (ingame) ap_abandon_mission();
            ap_set_mission(ps, pn);
            if (g_door_close && g_next_mode && g_door_flag) {
                if (!*g_door_flag) g_door_close();
                *g_next_mode = RR_MODE_LOADLEVEL;
                rr_log("ap: doors closing (flag %d, next %lu)", *g_door_flag, *g_next_mode);
            } else rr_log("ap: no doors (%p %p %p)", (void *)g_door_close, (void *)g_next_mode, (void *)g_door_flag);
        } else ap_choose_mission(ps, pn);
    }
}
