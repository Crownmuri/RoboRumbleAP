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
/* Money items: id -> amount for the shop (data.py MONEY) */
static const struct { int id, amount; } k_money[] = { { 5002, 500000 }, { 5003, 250000 }, { 5004, 1000000 }, { 5005, 2000000 } };
#define ROBO_FUNDS_ID 5001       /* Progressive Starting Robo Funds */
#define ROBO_FUNDS_STEP 400
#define COMBO_OFFSET 6000
#define SHOP_OFFSET 7000
#define SPY_OFFSET 7300
#define BONUS_OFFSET 7500   /* + 100 * side + 20 * kind (0 time, 1 base energy, 2 efficiency) + mission */

#define SHOP_SLOTS 22   /* 9 boosters, U1-U10, S1-S3 */

static CRITICAL_SECTION g_cs;
static int g_enabled, g_port;

/* received from the client (under g_cs) */
static int g_have_slot, g_side_mode, g_final_wins, g_start_side;
static int g_goal = 1, g_artifacts_needed = 10;   /* goal option (0-3) and the artifacts it needs */
static volatile LONG g_goal_done;                 /* alien_artifacts_only: reached, the epilogue is offered */
#define ARTIFACT_ID 5100   /* side mode 0 Red Star, 1 Terraflux, 2 both */
static char g_seed[64];
static int *g_items, g_nitems;
/* Unholy Alliance (ROWS line): builder slot side * 8 + i -> weapon / legs type; the game's own tables come from
   level1.ini [HeadsLegs] HeadA0-7, HeadB0-7, LegsA0-7, LegsB0-7 */
static int g_row_heads[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static int g_row_legs[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static int *g_tbl_heads, *g_tbl_legs;
static int *g_checked, g_nchecked;
static int g_choose_mission, g_choose_side;   /* MISSION side n, mission 0 = none */
static volatile LONG g_dirty;         /* items/checked changed */

/* the shop's goods as the client scouted them: "Rocket Launcher for Alice" (under g_cs) */
static char g_shop_label[2][SHOP_SLOTS][64];

/* the mission report's bonuses, set when a mission is won (time, efficiency, base energy) */
static int *g_bonus_time, *g_bonus_efficiency, *g_bonus_energy;

/* outgoing lines */
static char g_out[64][96];
static int g_nout;
static volatile LONG g_connected;

static const RRGame *G;
static int *g_spy_price;

/* ---------------------------------------------------------------- helpers */
/* dev mode (client /dev, host.yaml dev_mode): items given locally, and actions run on the game thread */
static int g_dev_items[256], g_ndev;
static volatile LONG g_dev_end;          /* 1 win / 2 lose the mission */
static volatile LONG g_dev_money_op;     /* 1 set / 2 add g_dev_money */
static volatile LONG g_dev_money;

static int count_item(int id)
{
    int n = 0;
    for (int i = 0; i < g_nitems; i++) n += g_items[i] == id;
    for (int i = 0; i < g_ndev; i++) n += g_dev_items[i] == id;
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

/* each side's campaign has its own shop checks */
static int slot_location(int slot, int side)
{
    int s = 50 * side;
    if (slot < 9) return SHOP_OFFSET + s + slot;
    if (slot < 17) return SHOP_OFFSET + 100 + s + slot - 9;
    if (slot >= 19) return SHOP_OFFSET + 200 + s + slot - 19;
    return 0;
}

static int mission_id(int side, int n) { return MISSION_OFFSET + 100 * side + n; }

static int side_played(int side) { return g_side_mode == 2 || g_side_mode == side; }

/* mission 1 is always open, 2-15 need their access item, the final mission a number of wins on that side */
static int mission_open(int side, int n)
{
    if (n == 1 && side == g_start_side) return 1;
    if (n < 16) return count_item(mission_id(side, n)) > 0;
    /* the final mission: its access item (final_mission_only, alien_artifacts_only), the artifacts
       (alien_artifacts_and_final_mission) or a number of wins (set_missions_and_final_mission) */
    if (g_goal == 0 || g_goal == 2) return count_item(mission_id(side, 16)) > 0;
    if (g_goal == 3) return count_item(ARTIFACT_ID) >= g_artifacts_needed;
    int wins = 0;
    for (int m = 1; m < 16; m++) wins += is_checked(mission_id(side, m));
    return wins >= g_final_wins;
}

static int campaign_side(void) { return *(int *)G->state == 1 ? 1 : 0; }

/* ---------------------------------------------------------------- applying the slot to the game */
static BYTE g_shop_written[SHOP_SLOTS];   /* owned[] as last written in the briefing */
static int g_shop_view;                   /* 1 = owned[] currently means "checked" (briefing) */
static int g_money_known;              /* MONEYSTATE came from the client */
static int g_money_store = -1;          /* the campaign's Money as the server has it (-1 = none yet) */
static int g_money_items = -1;          /* Money items added so far (-1 = none yet) */
static int g_money_sent_items = -1;
static volatile LONG g_money_restore = 1;   /* a save was loaded / a campaign started: the server's Money goes back in */
int rr_dev_infmoney(void);
static void money_tick(BYTE *S, DWORD mode);
void rr_money_restore(void) { InterlockedExchange(&g_money_restore, 1); }

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

    /* the parts in the builder's slots: the played side's slots as the slot data says, the other side's (the enemy's)
       as in the original game */
    if (g_tbl_heads && g_tbl_legs)
        for (int i = 0; i < 16; i++) {
            int mine = i / 8 == side;
            g_tbl_heads[i] = mine ? g_row_heads[i] : i;
            g_tbl_legs[i] = mine ? g_row_legs[i] : i;
        }
    /* parts the robot builder offers */
    for (int i = 0; i < 8; i++) {
        G->heads[i] = count_item(HEAD_OFFSET + g_row_heads[side * 8 + i]) ? '+' : '-';
        G->legs[i] = count_item(LEGS_OFFSET + g_row_legs[side * 8 + i]) ? '+' : '-';
    }
    G->heads[8] = G->legs[8] = 0;
    for (int i = 0; i < 9; i++) G->boosters[i] = count_item(BOOSTER_OFFSET + i) ? '+' : '-';
    G->boosters[9] = 0;

    BYTE *avail = S + 0x0C, *owned = S + 0x22;
    if (mode == RR_MODE_BRIEFING) {
        if (g_shop_view) {
            /* a good bought since the last look is a check */
            for (int s = 0; s < SHOP_SLOTS; s++)
                if (slot_location(s, side) && owned[s] && !g_shop_written[s]) check(slot_location(s, side));
        }
        for (int s = 0; s < SHOP_SLOTS; s++) {
            int loc = slot_location(s, side);
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

    money_tick(S, mode);
}

/* Money lives on the Archipelago server (data storage, via the client's MONEYSTATE / our MONEY lines), not in the
   game's saves: a loaded save or a new campaign gets the server's amount back, so Money received or earned after the
   last save isn't lost when the game closes without saving. Money items ("$500 000 Money") are added once each; the
   total added is kept on the server too. */
static void money_tick(BYTE *S, DWORD mode)
{
    if (!g_money_known || rr_dev_infmoney()) return;
    int *cash = (int *)(S + 8);
    if (g_money_restore && (mode == RR_MODE_BRIEFING || mode == RR_MODE_GAMEPLAY)) {
        g_money_restore = 0;
        if (g_money_store >= 0 && *cash != g_money_store) {
            rr_log("ap: Money %d -> %d (the server's)", *cash, g_money_store);
            *cash = g_money_store;
        }
    }
    if (g_money_restore) return;   /* a save is still loading */
    if (g_money_items < 0) {
        /* nothing on the server yet: an older version kept the items' total in rrpatch_ap.ini */
        char p[MAX_PATH];
        state_path(p, sizeof p);
        g_money_items = GetPrivateProfileIntA(g_seed, "money_applied", -1, p);
        if (g_money_items < 0) g_money_items = GetPrivateProfileIntA(g_seed, "shop_money_applied", 0, p) * 500000;
    }
    int items = 0;
    for (int i = 0; i < (int)(sizeof k_money / sizeof k_money[0]); i++) items += count_item(k_money[i].id) * k_money[i].amount;
    if (items > g_money_items) {
        *cash += items - g_money_items;
        g_money_items = items;
    }
    if (*cash != g_money_store || items != g_money_sent_items) {
        g_money_store = *cash;
        g_money_sent_items = g_money_items;
        send_line("MONEY %d %d", g_money_store, g_money_items);
    }
}

/* ---------------------------------------------------------------- for the dialogs (apdlg.c) */
/* shop slot: what buying it sends ("" if not scouted), whether it is a check and whether that is done */
void ap_shop_label(int slot, char *out, int size)
{
    out[0] = 0;
    if (slot < 0 || slot >= SHOP_SLOTS) return;
    EnterCriticalSection(&g_cs);
    int side = campaign_side();
    if (g_have_slot && slot_location(slot, side)) lstrcpynA(out, g_shop_label[side][slot], size);
    LeaveCriticalSection(&g_cs);
}
int ap_shop_slot_state(int slot)   /* -1 no check, 0 open, 1 done */
{
    if (slot < 0 || slot >= SHOP_SLOTS) return -1;
    EnterCriticalSection(&g_cs);
    int loc = g_have_slot ? slot_location(slot, campaign_side()) : 0;
    int state = !loc ? -1 : is_checked(loc) ? 1 : 0;
    LeaveCriticalSection(&g_cs);
    return state;
}
void ap_shop_opened(void) { send_line("SHOP %d", campaign_side()); }

int ap_side_mode(void) { return g_have_slot ? g_side_mode : -1; }
int ap_campaign_side(void) { return campaign_side(); }

int ap_mission_open(int side, int n)
{
    EnterCriticalSection(&g_cs);
    int open = g_have_slot && side_played(side) && mission_open(side, n);
    LeaveCriticalSection(&g_cs);
    return open;
}

/* the client's /mission: taken by the briefing (apdlg), which loads it right away like its own mission picker */
static volatile LONG g_client_pick;
int ap_take_client_pick(void) { return (int)InterlockedExchange(&g_client_pick, 0); }

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

static void (__cdecl *g_level_banner)(int level);   /* draws "LEVEL n" on the loading screen */
static DWORD *g_in_mission;   /* the briefing shows the in-mission menu while this is set */
static int *g_preset_side;    /* new campaign: side + 1 skips the faction screen (0 = the player picks) */
static int g_preset_orig;
static BYTE *g_boot_flag;
static int *g_saved_side;   /* side of the last session (rrumble.ini): while set, the intro is skipped */   /* set while booting: no intro, the first briefing opens on the robot's menu */
static DWORD g_prev_mode;
/* The mission report (briefing after a win) shows the records of mission "level - 1": it reads this instead of the
   level, which may have been changed to another mission since */
int g_report_level;

/* a mission left through the menu: no win, and the next briefing is a normal one */
void ap_abandon_mission(void)
{
    g_mission_running = 0;
    if (g_in_mission) *g_in_mission = 0;
}
int ap_mission_in_progress(void) { return g_mission_running != 0; }
int ap_goal_done(void) { return g_goal_done != 0; }

/* for traps.c */
void traps_install(int *local_player);
void traps_tick(void);
void traps_dev_fire(int trap);
void dev_install(int *local_player);
void dev_tick(void);
void dev_set(const char *what, int on);
void dev_alien(int type, int all);
void rr_add_robo_funds(int amount);
void rr_alien_item(int type);
void ap_alien_used(int type) { send_line("ALIENUSED %d", type); }
int ap_item_count(int id)
{
    EnterCriticalSection(&g_cs);
    int n = count_item(id);
    LeaveCriticalSection(&g_cs);
    return n;
}
const char *ap_seed(void) { return g_have_slot ? g_seed : NULL; }
static void queue_message(const char *text);
void ap_message(const char *text) { queue_message(text); }
/* Watch the epilogue was picked: the client completes the goal now (and the room releases) */
void ap_epilogue_watched(void) { send_line("EPILOGUE"); }
/* alien_artifacts_only: how many artifacts are still missing (-1 with another goal) */
int ap_artifacts_missing(void)
{
    if (!g_have_slot || g_goal != 2) return -1;
    EnterCriticalSection(&g_cs);
    int n = g_artifacts_needed - count_item(ARTIFACT_ID);
    LeaveCriticalSection(&g_cs);
    return n < 0 ? 0 : n;
}

/* the campaign goes to this mission at the next level load; the loading screen's door shows its number */
void ap_set_mission(int side, int n)
{
    EnterCriticalSection(&g_cs);
    *(int *)G->state = side;
    *(int *)(G->state + 4) = n - 1;
    g_choose_side = side;
    g_choose_mission = n;
    LeaveCriticalSection(&g_cs);
    if (g_level_banner) g_level_banner(n - 1);
}

static int side_has_open_mission(int side)
{
    for (int n = 1; n <= 16; n++) if (mission_open(side, n)) return 1;
    return 0;
}

/* The mission to play: the one picked in the menu; right after a win the won mission again (the game doesn't move on
   by itself, you pick the next one in the menu); else the game's current mission if it is unlocked (a loaded save),
   else the first unlocked mission not won yet. */
static int pick_mission(int won)
{
    int cur = *(int *)(G->state + 4) + 1, side = campaign_side();
    if (g_choose_mission && g_choose_side == side && mission_open(side, g_choose_mission)) return g_choose_mission;
    if (won) return won;
    if (cur >= 1 && cur <= 16 && mission_open(side, cur)) return cur;
    for (int n = 1; n <= 16; n++)
        if (mission_open(side, n) && !is_checked(mission_id(side, n))) return n;
    for (int n = 1; n <= 16; n++)
        if (mission_open(side, n)) return n;
    return 1;
}

static char *state_path(char *buf, size_t n);

/* A seed played for the first time: no check yet and not started in this game folder before */
static int fresh_seed(void)
{
    char p[MAX_PATH];
    return g_have_slot && g_nchecked == 0 &&
           !GetPrivateProfileIntA(g_seed, "started", 0, state_path(p, sizeof p));
}

static void on_mode(DWORD mode)
{
    /* Booting goes straight to a blank campaign (mode 9 without intro). For a fresh seed the game starts the way a
       new campaign does instead: the intro, then the first briefing of the slot's side. */
    /* The game boots through its intro screen (mode 2), which skips the story when a side is remembered from the
       last session and goes on to the boot campaign (mode 9). For a fresh seed the remembered side is cleared there,
       so the story plays once and leads into a new campaign; booting without the intro screen gets it as a
       replacement for mode 9. */
    static int booted;
    if ((mode == 2 || mode == 9) && !booted) {
        booted = 1;
        EnterCriticalSection(&g_cs);
        int fresh = fresh_seed();
        LeaveCriticalSection(&g_cs);
        if (fresh) {
            char p[MAX_PATH];
            WritePrivateProfileStringA(g_seed, "started", "1", state_path(p, sizeof p));
            rr_log("ap: fresh seed, the campaign starts with the intro");
            if (g_boot_flag) *g_boot_flag = 0;   /* a new campaign as from the menu, not the boot one */
            if (g_saved_side) *g_saved_side = 0; /* the intro only plays when no side is remembered */
            if (mode == 9) rr_replace_mode(2);
            return;
        }
    }
    /* Start a new campaign (faction screen next): with a slot the campaign is that of the slot's side, so the
       faction screen is skipped and the first briefing of that side follows */
    if (mode == 9 && g_preset_side) {
        int side = g_side_mode == 2 ? g_start_side : g_side_mode;
        *g_preset_side = g_have_slot ? side + 1 : g_preset_orig;
        if (g_have_slot) rr_log("ap: new campaign on side %d, faction screen skipped", side);
    }
    if (mode == 13 /* LOADGAME */ || mode == 9 /* new campaign */) rr_money_restore();
    if (!G || !g_have_slot) return;
    /* the remembered side was cleared for the fresh seed's intro; once in the campaign it is that of the campaign
       again, so the next boot doesn't play the intro a second time */
    if (mode == RR_MODE_BRIEFING && g_saved_side && *g_saved_side == 0) *g_saved_side = campaign_side() + 1;
    EnterCriticalSection(&g_cs);
    DWORD prev = g_prev_mode;
    g_prev_mode = mode;
    int *level = (int *)(G->state + 4);
    int won = 0;

    /* A won mission: the end-of-mission screen moves the campaign on to the next level and loads it. A restart
       keeps the level, and a mission picked in the menu clears g_mission_running first. */
    if (g_mission_running && mode == RR_MODE_LOADLEVEL && prev != RR_MODE_BRIEFING &&
        (*level == g_mission_running || *G->winflag == 1))
        won = g_mission_running;
    if (mode == RR_MODE_LOADLEVEL || mode == RR_MODE_MENU || mode == 11 /* RELOADLEVEL */ || mode == 13 /* LOADGAME */)
        g_mission_running = 0;

    /* only while a level loads: the level's map, funds and parts are set up there, so changing it later would mix
       two missions */
    if (!g_mission_running && mode == RR_MODE_LOADLEVEL) {
        int side = campaign_side();
        /* a campaign of a side this slot doesn't play, or one with nothing unlocked: the side that has */
        if (!side_played(side) || !side_has_open_mission(side)) {
            int to = side_played(g_start_side) && side_has_open_mission(g_start_side) ? g_start_side : 1 - side;
            if (to != side && side_played(to)) {
                rr_log("ap: campaign side %d isn't playable, switched to %d", side, to);
                *(int *)G->state = to;
            }
        }
        g_report_level = *level;   /* the game's own next mission: the report shows the records of the one before */
        int n = pick_mission(won);
        if (*level != n - 1) {
            rr_log("ap: mission %d -> %d", *level + 1, n);
            *level = n - 1;
            if (g_level_banner) g_level_banner(*level);
        }
        if (mode == RR_MODE_LOADLEVEL) {
            g_choose_mission = 0;
            InterlockedExchange(&g_client_pick, 0);   /* this load used the client's pick */
        }
    }
    int add_funds = 0;
    if (mode == RR_MODE_GAMEPLAY && !g_mission_running) {
        g_mission_running = *level + 1;
        g_choose_mission = 0;   /* the pick is used up: after a win the game's own next mission stands */
        add_funds = count_item(ROBO_FUNDS_ID) * ROBO_FUNDS_STEP;
    }
    int side_now = campaign_side();
    LeaveCriticalSection(&g_cs);
    /* Progressive Starting Robo Funds: the level's setup has given the mission's funds by now */
    if (add_funds) rr_add_robo_funds(add_funds);
    if (won) {
        rr_log("ap: mission %d of side %d won", won, side_now);
        check(mission_id(side_now, won));
        /* the report's bonuses: paid for beating (or matching) the mission's records */
        if (g_bonus_time && won < 16) {
            int bonus[3] = { *g_bonus_time, *g_bonus_energy, *g_bonus_efficiency };
            rr_log("ap: bonuses time %d energy %d efficiency %d", bonus[0], bonus[1], bonus[2]);
            for (int k = 0; k < 3; k++)
                if (bonus[k] > 0) check(BONUS_OFFSET + 100 * side_now + 20 * k + won);
        }
    }
    send_line("STATE %lu %d", mode, *level + 1);
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

/* ---------------------------------------------------------------- messages on screen */
/* MSG lines from the client go to the game's message line (where "robo ready" shows), during missions */
static void (__cdecl *g_show_message)(const char *text);
static char g_msgs[16][100];
static int g_nmsgs;

static void queue_message(const char *text)
{
    EnterCriticalSection(&g_cs);
    if (g_nmsgs == 16) { memmove(g_msgs[0], g_msgs[1], sizeof g_msgs[0] * 15); g_nmsgs = 15; }
    lstrcpynA(g_msgs[g_nmsgs++], text, sizeof g_msgs[0]);
    LeaveCriticalSection(&g_cs);
}

static void show_messages(void)
{
    static DWORD last;
    if (!g_show_message || g_gui_mode != RR_MODE_GAMEPLAY || !g_nmsgs) return;
    DWORD now = GetTickCount();
    if (now - last < 1500) return;
    last = now;
    char text[100];
    EnterCriticalSection(&g_cs);
    lstrcpynA(text, g_msgs[0], sizeof text);
    memmove(g_msgs[0], g_msgs[1], sizeof g_msgs[0] * 15);
    g_nmsgs--;
    LeaveCriticalSection(&g_cs);
    g_show_message(text);
}

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
    LONG end = InterlockedExchange(&g_dev_end, 0);
    if (end && G->winflag) {
        if (g_gui_mode == RR_MODE_GAMEPLAY) { *G->winflag = (DWORD)end; rr_log("dev: %s the mission", end == 1 ? "win" : "lose"); }
        else queue_message("Dev: not in a mission.");
    }
    LONG op = InterlockedExchange(&g_dev_money_op, 0);
    if (op) {
        int *money = (int *)(G->state + 8);
        *money = op == 1 ? (int)g_dev_money : *money + (int)g_dev_money;
        rr_log("dev: money %d", *money);
    }
    apdlg_tick();
    traps_tick();
    dev_tick();
    show_messages();
}

static BOOL (WINAPI *prev_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *prev_GetMessageA)(LPMSG, HWND, UINT, UINT);
static BOOL WINAPI ap_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { tick(); return prev_PeekMessageA(m, h, a, b, r); }
static BOOL WINAPI ap_GetMessageA(LPMSG m, HWND h, UINT a, UINT b) { tick(); return prev_GetMessageA(m, h, a, b); }


/* ---------------------------------------------------------------- the report after a mission */
/* The briefing starts a mission's report when it comes from a mission, except for the first mission, which it
   takes for a new campaign (the representative's welcome, no report). With missions in any order the first one
   can follow a mission too. Replaces "mov eax,[level]; test eax,eax; je first_mission". */
int *g_rep_level;
DWORD *g_rep_from_mission;
void *g_rep_resume, *g_rep_first;
__asm__(
    ".text\n"
    ".globl _report_stub\n"
    "_report_stub:\n"
    "    movl _g_rep_level, %eax\n"
    "    movl (%eax), %eax\n"
    "    testl %eax, %eax\n"
    "    jne 1f\n"
    "    movl _g_rep_from_mission, %edx\n"
    "    cmpl $0, (%edx)\n"
    "    jne 1f\n"
    "    jmp *_g_rep_first\n"
    "1:  jmp *_g_rep_resume\n");
void report_stub(void);

static BYTE *scan_code(int (*match)(const BYTE *p), int tail)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - tail;
        for (; p < end; p++) if (match(p)) return p;
    }
    return NULL;
}

/* mov eax,[level]; test eax,eax; je +x; mov edx,[from mission]; test edx,edx; je +5; cmp eax,10h; jne */
static int match_report(const BYTE *p)
{
    return p[0] == 0xA1 && *(BYTE **)(p + 1) == G->state + 4 && p[5] == 0x85 && p[6] == 0xC0 && p[7] == 0x74 &&
           p[9] == 0x8B && p[10] == 0x15 && p[15] == 0x85 && p[16] == 0xD2 && p[17] == 0x74 && p[18] == 0x05 &&
           p[19] == 0x83 && p[20] == 0xF8 && p[21] == 0x10 && p[22] == 0x75;
}

static void install_report(void)
{
    BYTE *p = scan_code(match_report, 32);
    if (!p) { rr_log("ap: briefing report code not found"); return; }
    g_rep_level = (int *)(G->state + 4);
    g_rep_from_mission = *(DWORD **)(p + 11);
    g_rep_first = p + 9 + (signed char)p[8];
    g_rep_resume = p + 9;
    if (rr_write_jmp(p, (void *)report_stub, 9)) rr_log("ap: report after any mission (%p)", p);
}

/* the message line: the code that announces "Console enabled" loads the text into ecx, then calls it */
static void find_message_fn(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *text = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !text; i++, sec++) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 16;
        for (; p < end; p++) if (!memcmp(p, "Console enabled", 15)) { text = p; break; }
    }
    if (!text) return;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_show_message; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 40;
        for (; p < end; p++) {
            if (p[0] != 0xB9 || *(BYTE **)(p + 1) != text) continue;
            for (BYTE *q = p + 5; q < p + 30; q++)
                if (q[0] == 0xE8) { g_show_message = (void *)(q + 5 + *(int *)(q + 1)); break; }
            break;
        }
    }
}

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
        int side = 0, final_wins = 10, start = 0, goal = 1, artifacts = 10;
        char seed[64] = "";
        sscanf(line + 5, "%d %d %d %63s %d %d", &side, &final_wins, &start, seed, &goal, &artifacts);
        EnterCriticalSection(&g_cs);
        if (strcmp(seed, g_seed)) { g_money_known = 0; g_money_store = g_money_items = g_money_sent_items = -1; }
        g_side_mode = side; g_final_wins = final_wins; g_start_side = start;
        g_goal = goal; g_artifacts_needed = artifacts;
        lstrcpynA(g_seed, seed, sizeof g_seed);
        g_have_slot = 1;
        LeaveCriticalSection(&g_cs);
        rr_log("ap: slot side %d, final mission after %d wins, seed %s", side, final_wins, seed);
    } else if (!strncmp(line, "ALIEN ", 6)) {
        rr_alien_item(atoi(line + 6));   /* an Alien Booster item to drop: ALIENUSED once it is on the map */
    } else if (!strncmp(line, "MONEYSTATE ", 11)) {
        int m = -1, i = -1;
        sscanf(line + 11, "%d %d", &m, &i);
        EnterCriticalSection(&g_cs);
        if (!g_money_known) {   /* the first one; later ones only echo what we sent */
            g_money_store = m;
            g_money_items = i;
            g_money_sent_items = i;
            g_money_known = 1;
            g_money_restore = 1;
        }
        LeaveCriticalSection(&g_cs);
        rr_log("ap: server Money %d, Money items added %d", m, i);
    } else if (!strncmp(line, "ROWS ", 5)) {
        int v[32], n = 0, used = 0;
        const char *q = line + 5;
        while (n < 32 && sscanf(q, "%d%n", &v[n], &used) == 1) { q += used; n++; }
        int ok = n == 32;
        for (int i = 0; i < 32 && ok; i++) ok = v[i] >= 0 && v[i] < 16;
        if (ok) {
            EnterCriticalSection(&g_cs);
            memcpy(g_row_heads, v, sizeof g_row_heads);
            memcpy(g_row_legs, v + 16, sizeof g_row_legs);
            LeaveCriticalSection(&g_cs);
            int mixed = 0;
            for (int i = 0; i < 16; i++) mixed |= v[i] != i || v[16 + i] != i;
            rr_log("ap: builder slots %s", mixed ? "mixed (Unholy Alliance)" : "as in the original game");
        }
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
        if (g_choose_mission) InterlockedExchange(&g_client_pick, (side ? 100 : 0) + n);
        rr_log("ap: next mission %d of side %d", n, side);
    } else if (!strcmp(line, "GOAL")) {
        if (!InterlockedExchange(&g_goal_done, 1)) {
            rr_log("ap: goal reached");
            queue_message("Archipelago goal reached! The robot has the epilogue for you.");
        }
    } else if (!strncmp(line, "SHOPITEM ", 9)) {
        /* SHOPITEM side slot flags text: what buying that slot in that side's shop sends */
        int side = -1, slot = -1, flags = 0, used = 0;
        if (sscanf(line + 9, "%d %d %d %n", &side, &slot, &flags, &used) >= 3 && side >= 0 && side < 2 &&
            slot >= 0 && slot < SHOP_SLOTS && used) {
            EnterCriticalSection(&g_cs);
            lstrcpynA(g_shop_label[side][slot], line + 9 + used, sizeof g_shop_label[0][0]);
            LeaveCriticalSection(&g_cs);
        }
    } else if (!strncmp(line, "DEV ", 4)) {
        /* DEV GIVE id | DEV CLEAR | DEV TRAP t | DEV WIN | DEV LOSE | DEV MONEY n | DEV ADDMONEY n |
           DEV SET GOD|NOLOSE|FUNDS|INFMONEY 0/1 | DEV ALIEN type [ALL] */
        const char *a = line + 4;
        int v = 0;
        if (sscanf(a, "GIVE %d", &v) == 1) {
            EnterCriticalSection(&g_cs);
            if (g_ndev < (int)(sizeof g_dev_items / sizeof g_dev_items[0])) g_dev_items[g_ndev++] = v;
            LeaveCriticalSection(&g_cs);
            rr_log("dev: give item %d", v);
        } else if (!strcmp(a, "CLEAR")) {
            EnterCriticalSection(&g_cs);
            g_ndev = 0;
            LeaveCriticalSection(&g_cs);
            rr_log("dev: given items cleared");
        } else if (sscanf(a, "TRAP %d", &v) == 1) {
            traps_dev_fire(v);
        } else if (!strcmp(a, "WIN")) {
            InterlockedExchange(&g_dev_end, 1);
        } else if (!strcmp(a, "LOSE")) {
            InterlockedExchange(&g_dev_end, 2);
        } else if (!strncmp(a, "SET ", 4)) {
            char what[16] = "";
            if (sscanf(a + 4, "%15s %d", what, &v) == 2) dev_set(what, v);
        } else if (sscanf(a, "ALIEN %d", &v) == 1) {
            dev_alien(v, strstr(a, " ALL") != NULL);
        } else if (sscanf(a, "MONEY %d", &v) == 1) {
            g_dev_money = v; InterlockedExchange(&g_dev_money_op, 1);
        } else if (sscanf(a, "ADDMONEY %d", &v) == 1) {
            g_dev_money = v; InterlockedExchange(&g_dev_money_op, 2);
        }
    } else if (!strncmp(line, "MSG ", 4)) {
        rr_log("ap: %s", line + 4);
        queue_message(line + 4);
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

/* The loader of level1.ini [HeadsLegs]: mov edi,"HeadA%d"; ...; mov [ebx*4+table],eax. The B row follows the A row. */
static int *find_row_table(const char *key)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t klen = strlen(key) + 1;
    BYTE *str = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !str; i++, sec++) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - klen;
        for (; p < end; p++) if (!memcmp(p, key, klen)) { str = p; break; }
    }
    if (!str) return NULL;
    BYTE pat[5] = { 0xBF };
    memcpy(pat + 1, &str, 4);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 64;
        for (; p < end; p++) {
            if (memcmp(p, pat, 5)) continue;
            for (int k = 5; k < 56; k++)
                if (p[k] == 0x89 && p[k + 1] == 0x04 && p[k + 2] == 0x9D) return *(int **)(p + k + 3);
        }
    }
    return NULL;
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
    g_tbl_heads = find_row_table("HeadA%d");
    g_tbl_legs = find_row_table("LegsA%d");
    if (!g_tbl_heads || !g_tbl_legs) { g_tbl_heads = g_tbl_legs = NULL; rr_log("ap: builder slot tables not found, no Unholy Alliance"); }
    else rr_log("ap: builder slot tables %p %p", (void *)g_tbl_heads, (void *)g_tbl_legs);

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
        traps_install(*(int **)(built - 14));   /* cmp ebp,[local player] before it */
        dev_install(*(int **)(built - 14));
        g_built_resume = built + 6;
        rr_write_jmp(built, (void *)built_stub, 6);
    } else rr_log("ap: robot spawn code not found, no build checks");

    /* the report's bonuses, summed into the money: mov eax,[time]; mov edx,[efficiency]; add eax,edx;
       mov edx,[energy]; add eax,edx; mov edx,[money]; add eax,edx */
    {
        BYTE pat[] = { 0xA1, 0, 0, 0, 0, 0x8B, 0x15, 0, 0, 0, 0, 0x03, 0xC2, 0x8B, 0x15, 0, 0, 0, 0, 0x03, 0xC2,
                       0x8B, 0x15, 0, 0, 0, 0, 0x03, 0xC2 };
        *(DWORD *)(pat + 23) = (DWORD)G->state + 8;
        BYTE *base = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_bonus_time; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - sizeof pat;
            for (; p < end; p++) {
                int k = 0;
                while (k < (int)sizeof pat && ((k >= 1 && k < 5) || (k >= 7 && k < 11) || (k >= 15 && k < 19) ||
                                               p[k] == pat[k])) k++;
                if (k == (int)sizeof pat) {
                    g_bonus_time = *(int **)(p + 1);
                    g_bonus_efficiency = *(int **)(p + 7);
                    g_bonus_energy = *(int **)(p + 15);
                    break;
                }
            }
        }
        rr_log("ap: report bonuses %p %p %p", (void *)g_bonus_time, (void *)g_bonus_efficiency, (void *)g_bonus_energy);
    }

    /* the report's three record lookups: mov reg,[side]; mov reg,[level]; shl reg,4; ...; lea reg,[..-1];
       mov reg,[reg*4+records] -> read g_report_level instead of the level */
    {
        BYTE *base = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        DWORD side_addr = (DWORD)G->state, level_addr = side_addr + 4;
        BYTE *sites[3];
        int found = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 64;
            for (; p < end; p++) {
                int a = (p[0] == 0x8B && p[1] == 0x15) ? 2 : p[0] == 0xA1 ? 1 : 0;
                if (!a || *(DWORD *)(p + a) != side_addr) continue;
                BYTE *q = p + a + 4;
                if (q[0] != 0x8B || (q[1] != 0x0D && q[1] != 0x15) || *(DWORD *)(q + 2) != level_addr) continue;
                if (q[6] != 0xC1 || (q[7] != 0xE2 && q[7] != 0xE0) || q[8] != 0x04) continue;
                int lea = 0, load = 0;
                for (int k = 9; k < 49; k++) {
                    if (!lea && q[k] == 0x8D && (q[k + 1] == 0x54 || q[k + 1] == 0x44) && q[k + 3] == 0xFF) lea = k;
                    if (lea && k > lea && q[k] == 0x8B && (q[k + 1] == 0x14 || q[k + 1] == 0x0C) &&
                        (q[k + 2] == 0x95 || q[k + 2] == 0x85)) { load = k; break; }
                }
                if (load && found < 3) sites[found] = q + 2;
                if (load) found++;
            }
        }
        if (found == 3) {
            for (int i = 0; i < 3; i++) {
                DWORD old;
                VirtualProtect(sites[i], 4, PAGE_EXECUTE_READWRITE, &old);
                *(DWORD *)sites[i] = (DWORD)&g_report_level;
                VirtualProtect(sites[i], 4, old, &old);
            }
            rr_log("ap: mission report reads the played mission");
        } else rr_log("ap: mission report code not found (%d)", found);
    }

    BYTE *spy = rr_find_unique((const BYTE *)"\x2b\xc2\xba\xff\xff\xff\xff\x51\x89\x15", 10);
    if (spy && *(BYTE **)(spy + 15) == G->state + 8) g_spy_price = *(int **)(spy + 10);
    else rr_log("ap: spy code not found, no spy checks");

    /* the load-game screen: mov eax,[level]; mov [esp],eax; call banner; add esp,4; mov eax,8; ret */
    {
        BYTE *base = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_level_banner; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 24;
            for (; p < end; p++)
                if (p[0] == 0xA1 && *(BYTE **)(p + 1) == G->state + 4 && p[5] == 0x89 && p[6] == 0x04 &&
                    p[7] == 0x24 && p[8] == 0xE8 && p[13] == 0x83 && p[14] == 0xC4 && p[15] == 0x04 &&
                    p[16] == 0xB8 && p[17] == 0x08 && p[18] == 0 && p[19] == 0 && p[20] == 0 && p[21] == 0xC3) {
                    g_level_banner = (void *)(p + 13 + *(int *)(p + 9));
                    /* the same load-game code first clears the in-mission flag: xor eax,eax; mov [flag],eax */
                    if (p[-17] == 0x33 && p[-16] == 0xC0 && p[-15] == 0xA3) g_in_mission = *(DWORD **)(p - 14);
                    break;
                }
        }
        if (!g_level_banner) rr_log("ap: level banner not found");
    }

    /* new campaign: mov eax,[preset side]; test eax,eax; jne preset; mov al,0; mov edx,[..]; test edx,edx; mov [esp+..],al */
    {
        BYTE *base = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_preset_side; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE *p = base + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - 32;
            for (; p < end; p++)
                if (p[0] == 0xA1 && p[5] == 0x85 && p[6] == 0xC0 && p[7] == 0x0F && p[8] == 0x85 && p[13] == 0xB0 &&
                    p[14] == 0 && p[15] == 0x8B && p[16] == 0x15 && p[21] == 0x85 && p[22] == 0xD2 && p[23] == 0x88 &&
                    p[24] == 0x84 && p[25] == 0x24) {
                    g_preset_side = *(int **)(p + 1);
                    g_preset_orig = *g_preset_side;
                    /* just before it: mov dl,[booting]; test dl,dl; mov [..],eax; jne */
                    if (p[-19] == 0x8A && p[-18] == 0x15 && p[-13] == 0x84 && p[-12] == 0xD2) {
                        g_boot_flag = *(BYTE **)(p - 17);
                        BYTE *boot = p + *(int *)(p - 4);   /* the jne's target: mov edx,[saved side] */
                        if (boot[0] == 0x8B && boot[1] == 0x15) g_saved_side = *(int **)(boot + 2);
                    }
                    break;
                }
        }
        if (!g_preset_side) rr_log("ap: faction preset not found, the faction screen stays");
    }

    install_report();
    find_message_fn();
    if (!g_show_message) rr_log("ap: message line not found, no messages in the game");

    HMODULE exe = GetModuleHandleA(NULL);
    prev_PeekMessageA = rr_hook_iat(exe, "USER32.dll", "PeekMessageA", (void *)ap_PeekMessageA);
    prev_GetMessageA = rr_hook_iat(exe, "USER32.dll", "GetMessageA", (void *)ap_GetMessageA);
    CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, NULL));
    rr_log("ap: waiting for the client on port %d (order %p, built %p, spy price %p)", g_port, order, built, g_spy_price);
}
