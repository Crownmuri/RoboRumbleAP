"""
Static tables shared by generation and the client.

AP ids are derived from the game's own indices, so the game patch (patch/src) can compute them without a lookup
table:
    heads (weapons)  HEAD_OFFSET + head index 0-15      (index = HeadA0-7 / HeadB0-7 of level1.ini)
    legs             LEGS_OFFSET + legs index 0-15
    boosters         BOOSTER_OFFSET + booster index 0-8 (order of the Boosters mask = the robot builder's order)
    mission access   MISSION_OFFSET + mission number 2-16 (item)
    mission complete MISSION_OFFSET + mission number 1-16 (location)
    combo build      COMBO_OFFSET + head index * 16 + legs index (location, option combo_builds)
    base upgrades    UPGRADE_OFFSET + index 0-7 (item; order of the engineer's menu)
    inventions       INVENTION_OFFSET + index 0-2 (item)
    shop purchases   SHOP_OFFSET + booster index 0-8, + 100 + upgrade index, + 200 + invention index (locations)
    spy hints        SPY_OFFSET + mission number 1-16 (location: buying the spy's report before that mission)
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

FILLER: Dict[int, str] = {
    FILLER_OFFSET + 1: "Mission Funds Bonus",   # more starting funds in the next mission
    FILLER_OFFSET + 2: "Shop Money",            # money to spend in the shop between missions
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


def mission_access(number: int) -> str:
    return f"Mission {number} Access"


def mission_complete(number: int) -> str:
    return f"Mission {number} Complete"


def build_head(name: str) -> str:
    return f"Build {name}"


def build_legs(name: str) -> str:
    return f"Build {name}"


def build_booster(name: str) -> str:
    return f"Build with {name}"


def build_combo(head: str, legs: str) -> str:
    return f"Build {head} + {legs}"


def side_combos(side: int) -> List[str]:
    return [build_combo(h, l) for h in side_heads(side) for l in side_legs(side)]


def shop_booster(name: str) -> str:
    return f"Shop: {name}"


def shop_upgrade(name: str) -> str:
    return f"Shop: {name}"


def shop_invention(name: str) -> str:
    return f"Shop: {name}"


def spy_hint(number: int) -> str:
    return f"Spy Report Before Mission {number}"


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
    for number in range(2, MISSIONS + 1):
        table[mission_access(number)] = MISSION_OFFSET + number
    for i, name in enumerate(UPGRADES):
        table[name] = UPGRADE_OFFSET + i
    for i, name in enumerate(INVENTIONS):
        table[name] = INVENTION_OFFSET + i
    table[ARTIFACT] = ARTIFACT_ID
    for item_id, name in FILLER.items():
        table[name] = item_id
    return table


def location_table() -> Dict[str, int]:
    table: Dict[str, int] = {}
    for number in range(1, MISSIONS + 1):
        table[mission_complete(number)] = MISSION_OFFSET + number
    for i, name in enumerate(HEADS):
        table[build_head(name)] = HEAD_OFFSET + i
    for i, name in enumerate(LEGS):
        table[build_legs(name)] = LEGS_OFFSET + i
    for i, name in enumerate(BOOSTERS):
        table[build_booster(name)] = BOOSTER_OFFSET + i
    for h, head in enumerate(HEADS):
        for l, legs in enumerate(LEGS):
            if h // 8 == l // 8:
                table[build_combo(head, legs)] = COMBO_OFFSET + h * 16 + l
    for i, name in enumerate(BOOSTERS):
        table[shop_booster(name)] = SHOP_OFFSET + i
    for i, name in enumerate(UPGRADES):
        table[shop_upgrade(name)] = SHOP_OFFSET + 100 + i
    for i, name in enumerate(INVENTIONS):
        table[shop_invention(name)] = SHOP_OFFSET + 200 + i
    for number in range(1, MISSIONS + 1):
        table[spy_hint(number)] = SPY_OFFSET + number
    return table
