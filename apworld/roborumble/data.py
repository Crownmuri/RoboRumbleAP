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
The "Build" locations use the same ids as the part items they belong to.
"""
from typing import Dict, List, Tuple

GAME_NAME = "RoboRumble"

HEAD_OFFSET = 1000
LEGS_OFFSET = 2000
BOOSTER_OFFSET = 3000
MISSION_OFFSET = 4000
FILLER_OFFSET = 5000
COMBO_OFFSET = 6000

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
# Order of the Boosters mask; the last three are pick-two-of-three in the robot builder.
BOOSTERS: Tuple[str, ...] = (
    "Speed Booster", "Turn Speed Booster", "Fire Power Booster", "Fire Range Booster", "Fire Frequency Booster",
    "Shield", "Camouflage", "Maximum Defense Booster", "Maximum Weapon Booster",
)

FILLER: Dict[int, str] = {
    FILLER_OFFSET + 1: "Extra Funds",
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
    return table
