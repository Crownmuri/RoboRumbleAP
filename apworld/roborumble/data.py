"""
Static tables shared by generation and the client.

AP ids are derived from the game's own indices, so the game patch (patch/src) can compute them without a lookup
table:
    heads (weapons)  HEAD_OFFSET + head index 0-15      (index = HeadA0-7 / HeadB0-7 of level1.ini)
    legs             LEGS_OFFSET + legs index 0-15
    boosters         BOOSTER_OFFSET + booster index 0-8 (order of the Boosters mask = the robot builder's order)
    mission access   MISSION_OFFSET + 100 * side + mission number 1-15 (item; mission 1 only for the second side
                     with side: both; mission 16 only with the goals final_mission_only and alien_artifacts_only,
                     it opens after a number of wins, option final_mission_wins)
    mission complete MISSION_OFFSET + 100 * side + mission number 1-16 (location)
    combo build      COMBO_OFFSET + head index * 16 + legs index (location, option combo_builds; any two parts, since
                     Unholy Alliance can put both sides' parts on one side)
    base upgrades    UPGRADE_OFFSET + index 0-7 (item; order of the engineer's menu)
    inventions       INVENTION_OFFSET + index 0-2 (item)
    shop purchases   SHOP_OFFSET + 50 * side + booster slot 0-8, + 100 + upgrade slot 0-7, + 200 + invention 0-2
                     (locations; each side's campaign has its own shop)
    spy hints        SPY_OFFSET + 100 * side + mission number 1-16 (location: the spy's report before that mission)
    report bonuses   BONUS_OFFSET + 100 * side + 20 * kind + mission number 1-15 (kind 0 time, 1 base energy,
                     2 efficiency: the mission report's bonus for matching or beating that record)
side: 0 = Red Star (campaign A), 1 = Terraflux (campaign B)
The "Build" locations use the same ids as the part items they belong to.

The game's save state (devui: state block S) keeps 22 "available" and 22 "owned" flags for the shop:
9 boosters, 10 base upgrade slots, 3 secret inventions.
"""
from typing import Dict, List, Tuple

GAME_NAME = "RoboRumble"

HEAD_OFFSET = 1000
LEGS_OFFSET = 2000
BOOSTER_OFFSET = 3000
MISSION_OFFSET = 4000
FILLER_OFFSET = 5000
COMBO_OFFSET = 6000
SHOP_OFFSET = 7000
SPY_OFFSET = 7300
BONUS_OFFSET = 7500
UPGRADE_OFFSET = 3100
INVENTION_OFFSET = 3200
ARTIFACT_ID = 5100

MISSIONS = 16

SIDES = ("Red Star", "Terraflux")   # side A, side B

# Names from nls\lang.ini (100-115 heads, 200-215 legs). Side A uses index 0-7, side B 8-15.
HEADS: Tuple[str, ...] = (
    "Chain Gun", "Rocket Launcher", "Homing Missile", "Mine Spreader", "Oiler", "Magneton", "Force Field", "Nuke",
    "Laser", "Pulse Gun", "Eraser", "Buzzer", "Nitrogrenade", "Disrupter", "Whirlwind", "Infector",
)
LEGS: Tuple[str, ...] = (
    "2-Legs A", "Heavy 4-Legs", "Kangaroo", "Caterpillar", "4-Tires", "Amphibian", "Propeller", "Snail",
    "2-Legs B", "Teleporter", "Jet-Pack", "Spider Legs", "Jet-Car", "Hovercraft", "Antigraviton", "Cube",
)
# Weapons that do no damage on their own (utility): a robot army needs another weapon besides them
SUPPORT_HEADS = ("Oiler", "Nitrogrenade", "Disrupter")


def side_attack_heads(side: int) -> List[str]:
    return [h for h in side_heads(side) if h not in SUPPORT_HEADS]


# Order of the Boosters mask and of the shop's booster slots (nls\lang.ini 732-740). In the original game
# Maximum Weapon needs Fire Power + Fire Range + Fire Frequency, Maximum Defense needs Speed + Turn Speed + Shield,
# and Camouflage excludes both Maximum boosters (pick two of three).
BOOSTERS: Tuple[str, ...] = (
    "Speed Booster", "Turn Speed Booster", "Fire Power Booster", "Fire Range Booster", "Fire Frequency Booster",
    "Shield", "Camouflage", "Maximum Weapon Booster", "Maximum Defense Booster",
)
# The engineer's "base enhancement" menu (nls\lang.ini 770-777) = shop slots U1-U8 (U9/U10 unused). They come in
# pairs: a new campaign offers U1, U3, U5 and U7; buying one makes the next slot (U2, U4, U6, U8) available.
UPGRADES: Tuple[str, ...] = (
    "Faster Production", "Cheaper Production", "Base Energy +10%", "Base Regeneration",
    "Stronger Energy Explosions", "Wider Energy Explosions", "Explosions Recharge Robos", "Enemy Explosions Spare 1%",
)
# The engineer's secret inventions, slots S1-S3, sold in this order ("surprise me")
INVENTIONS: Tuple[str, ...] = (
    "Robo Intelligence Enhancer",   # robos target the enemy with the least energy first
    "Robo Spawn Protection",        # indestructible for 10 s after production
    "Robo Auto-Repair",             # energy refills after a minute without being shot
)
ARTIFACT = "Alien Artifact"

# Robo Funds pay for robots during a mission; Money pays for the shop between missions.
ROBO_FUNDS = "Progressive Starting Robo Funds"   # +ROBO_FUNDS_STEP Robo Funds at the start of every mission, per copy
ROBO_FUNDS_ID = FILLER_OFFSET + 1
ROBO_FUNDS_STEP = 400
# Money for the shop between missions; the name says how much, like the game shows it ("$500 000")
MONEY: Dict[int, int] = {
    FILLER_OFFSET + 3: 250000,
    FILLER_OFFSET + 2: 500000,
    FILLER_OFFSET + 4: 1000000,
    FILLER_OFFSET + 5: 2000000,
}
MONEY_WEIGHTS = (4, 4, 2, 1)   # in the order of MONEY: small amounts are more common


def money_name(amount: int) -> str:
    return f"${amount:,} Money".replace(",", " ")


FILLER: Dict[int, str] = {item_id: money_name(amount) for item_id, amount in MONEY.items()}

# Alien boosters to drop on the map yourself (client: /alien or the RoboRumble tab), next to your selected robot; the
# robot that drives over it gets it. Order = the game's booster tile types 1-4.
ALIEN_OFFSET = 5010
ALIEN_BOOSTERS: Dict[int, str] = {
    ALIEN_OFFSET + 0: "Alien Speed Booster",          # faster robot
    ALIEN_OFFSET + 1: "Alien Defense Booster",        # takes a fifth of the damage
    ALIEN_OFFSET + 2: "Alien Weapon Booster",         # stronger weapon
    ALIEN_OFFSET + 3: "Alien Intelligence Booster",   # smarter targeting
}

# Traps hit a random robot of the player during a mission (the patch keeps them until the next mission otherwise).
TRAP_OFFSET = 5200
TRAPS: Dict[int, str] = {
    TRAP_OFFSET + 0: "Oil Trap",         # an enemy oil slick under the robot
    TRAP_OFFSET + 1: "Freeze Trap",      # frozen like a Nitrogrenade hit
    TRAP_OFFSET + 2: "Disruptor Trap",   # malfunctions like a Disrupter hit
    TRAP_OFFSET + 3: "Mine Trap",        # destroyed after 30 seconds
    TRAP_OFFSET + 4: "Nuke Trap",        # the robot goes off like a Nuke robot (own nuke)
}

# What each campaign mission lets you build in the original game ('+' = available), per side, taken from the
# missions' setup code (rr_soft.exe table at 0x52F4F0). Index i of a mask = part i of that side (HeadA/B i).
VANILLA_HEADS: Dict[int, Tuple[str, ...]] = {
    0: ("++------", "+++-----", "+++-----", "++++----", "++++-+--", "++++-+--", "++++-+--", "++++++--",
        "++++++--", "+++++++-", "+++++++-", "+++++++-", "++++++++", "++++++++", "++++++++", "++++++++"),
    1: ("+-+-----", "++------", "+++-----", "++-+----", "++-++---", "++-+----", "++-+++--", "--++-+--",
        "++++++--", "++++++--", "++++++--", "++++++--", "++++++-+", "++++++-+", "++++++++", "++++++++"),
}
VANILLA_LEGS: Dict[int, Tuple[str, ...]] = {
    0: ("+-------", "+-------", "+--+----", "+--+----", "++-+----", "++-+----", "++-++---", "++-++---",
        "++-+----", "++-+++--", "++-+++--", "++-+++--", "++++++-+", "++++++++", "++++++++", "++++++++"),
    1: ("+-------", "+-------", "+--+----", "+--+----", "++-+----", "++-++---", "++-++---", "---++---",
        "++-+----", "++-+----", "++-+----", "++-+----", "+----++-", "+-++++--", "++++++++", "++++++++"),
}


def side_heads(side: int) -> List[str]:
    return list(HEADS[side * 8:side * 8 + 8])


def side_legs(side: int) -> List[str]:
    return list(LEGS[side * 8:side * 8 + 8])


FINAL_MISSION = MISSIONS


def mission_access(side: int, number: int) -> str:
    return f"{SIDES[side]} Mission {number} Access"


def tag(side: int) -> str:
    return f"[{SIDES[side]}]"


def mission_complete(side: int, number: int) -> str:
    return f"{tag(side)} Mission {number} - Clear"


def mission_id(side: int, number: int) -> int:
    return MISSION_OFFSET + 100 * side + number


def final_goal(side: int) -> str:
    return f"{tag(side)} Mission {FINAL_MISSION} - Defeat the Aliens"


def build_head(name: str) -> str:
    return f"Build {name}"


def build_legs(name: str) -> str:
    return f"Build {name}"


def build_booster(name: str) -> str:
    return f"Build with {name}"


def build_combo(head: str, legs: str) -> str:
    return f"Build {head} + {legs}"


def side_combos(side: int) -> List[str]:
    """the combos of a side's own parts in the original game (with Unholy Alliance the world knows the sides' parts)"""
    return [build_combo(h, l) for h in side_heads(side) for l in side_legs(side)]


def shop_booster(side: int, i: int) -> str:
    return f"{tag(side)} Booster Shop Slot {i + 1}"


def shop_upgrade(side: int, i: int) -> str:
    return f"{tag(side)} Base Upgrade Shop Slot {i + 1}"


def shop_invention(side: int, i: int) -> str:
    return f"{tag(side)} Secret Invention #{i + 1}"


def shop_locations(side: int) -> List[str]:
    return ([shop_booster(side, i) for i in range(len(BOOSTERS))] + [shop_upgrade(side, i) for i in range(len(UPGRADES))]
            + [shop_invention(side, i) for i in range(len(INVENTIONS))])


def shop_slots() -> Dict[int, Tuple[int, int]]:
    """shop location id -> (side, the game's shop slot: boosters 0-8, base upgrades 9-16, inventions 19-21)"""
    slots: Dict[int, Tuple[int, int]] = {}
    for side in (0, 1):
        for i in range(len(BOOSTERS)):
            slots[SHOP_OFFSET + 50 * side + i] = (side, i)
        for i in range(len(UPGRADES)):
            slots[SHOP_OFFSET + 100 + 50 * side + i] = (side, 9 + i)
        for i in range(len(INVENTIONS)):
            slots[SHOP_OFFSET + 200 + 50 * side + i] = (side, 19 + i)
    return slots


def spy_hint(side: int, number: int) -> str:
    return f"{tag(side)} Mission {number} - Spy Report"


BONUS_KINDS = ("Time", "Energy", "Efficiency")
BONUS_MISSIONS = MISSIONS - 1   # the final mission has no report


def bonus(side: int, number: int, kind: int) -> str:
    return f"{tag(side)} Mission {number} - {BONUS_KINDS[kind]} Bonus"


def bonus_id(side: int, number: int, kind: int) -> int:
    return BONUS_OFFSET + 100 * side + 20 * kind + number


def starting_parts(side: int) -> Tuple[List[str], List[str]]:
    """The heads and legs the first mission of a side gives in the original game."""
    heads = [side_heads(side)[i] for i, c in enumerate(VANILLA_HEADS[side][0]) if c == "+"]
    legs = [side_legs(side)[i] for i, c in enumerate(VANILLA_LEGS[side][0]) if c == "+"]
    return heads, legs


def vanilla_counts(side: int, mission: int) -> Tuple[int, int]:
    """How many heads and legs mission `mission` (1-16) offers in the original game."""
    return VANILLA_HEADS[side][mission - 1].count("+"), VANILLA_LEGS[side][mission - 1].count("+")


def item_table() -> Dict[str, int]:
    table: Dict[str, int] = {}
    for i, name in enumerate(HEADS):
        table[name] = HEAD_OFFSET + i
    for i, name in enumerate(LEGS):
        table[name] = LEGS_OFFSET + i
    for i, name in enumerate(BOOSTERS):
        table[name] = BOOSTER_OFFSET + i
    for side in (0, 1):
        for number in range(1, FINAL_MISSION + 1):
            table[mission_access(side, number)] = mission_id(side, number)
    for i, name in enumerate(UPGRADES):
        table[name] = UPGRADE_OFFSET + i
    for i, name in enumerate(INVENTIONS):
        table[name] = INVENTION_OFFSET + i
    table[ARTIFACT] = ARTIFACT_ID
    table[ROBO_FUNDS] = ROBO_FUNDS_ID
    for item_id, name in FILLER.items():
        table[name] = item_id
    for item_id, name in TRAPS.items():
        table[name] = item_id
    for item_id, name in ALIEN_BOOSTERS.items():
        table[name] = item_id
    return table


def location_table() -> Dict[str, int]:
    table: Dict[str, int] = {}
    for side in (0, 1):
        for number in range(1, MISSIONS + 1):
            table[mission_complete(side, number)] = mission_id(side, number)
    for i, name in enumerate(HEADS):
        table[build_head(name)] = HEAD_OFFSET + i
    for i, name in enumerate(LEGS):
        table[build_legs(name)] = LEGS_OFFSET + i
    for i, name in enumerate(BOOSTERS):
        table[build_booster(name)] = BOOSTER_OFFSET + i
    for h, head in enumerate(HEADS):
        for l, legs in enumerate(LEGS):
            table[build_combo(head, legs)] = COMBO_OFFSET + h * 16 + l
    for side in (0, 1):
        for i in range(len(BOOSTERS)):
            table[shop_booster(side, i)] = SHOP_OFFSET + 50 * side + i
        for i in range(len(UPGRADES)):
            table[shop_upgrade(side, i)] = SHOP_OFFSET + 100 + 50 * side + i
        for i in range(len(INVENTIONS)):
            table[shop_invention(side, i)] = SHOP_OFFSET + 200 + 50 * side + i
    for side in (0, 1):
        for number in range(1, MISSIONS + 1):
            table[spy_hint(side, number)] = SPY_OFFSET + 100 * side + number
    for side in (0, 1):
        for number in range(1, BONUS_MISSIONS + 1):
            for kind in range(len(BONUS_KINDS)):
                table[bonus(side, number, kind)] = bonus_id(side, number, kind)
    return table
