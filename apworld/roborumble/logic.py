"""
Reads logic.txt: per mission, what logic expects you to have before it sends you in (format: see logic.txt).

A requirement is a list of options (written "A | B"), an option a list of terms that are all needed ("A & B").
A term is (name, count): an item and how many of it ("2x Progressive Starting Robo Funds"), or a kind of part and how
many different ones ("2 damage weapons"); the kinds are KINDS.
"""
import pkgutil
import re
from dataclasses import dataclass, field
from typing import Dict, List, Tuple, Union

from . import data

Term = Tuple[str, int]   # (item name or kind, count)
Requirement = List[List[Term]]
KINDS = ("weapon", "damage weapon", "legs")


@dataclass
class MissionLogic:
    base: bool = True
    need: List[Requirement] = field(default_factory=list)
    recommended: List[Requirement] = field(default_factory=list)


class LogicError(Exception):
    pass


_HEADER = re.compile(r"^==\s*(Red Star|Terraflux)\s+Mission\s+(\d+)\s*==$", re.IGNORECASE)
_COUNT = re.compile(r"^(?:(\d+)\s+|any\s+)?(damage weapons?|weapons?|legs)$", re.IGNORECASE)
_TIMES = re.compile(r"^(\d+)\s*x\s+(.+)$", re.IGNORECASE)


def _names(side: int) -> Dict[str, str]:
    """lower-case name -> item name, for what can be written in a mission of that side"""
    names = {n.lower(): n for n in data.side_heads(side) + data.side_legs(side)}
    for n in data.BOOSTERS + data.UPGRADES + data.INVENTIONS:
        names[n.lower()] = n
    names["2-legs"] = data.side_legs(side)[0]
    names[data.ROBO_FUNDS.lower()] = data.ROBO_FUNDS
    return names


def _term(text: str, side: int, where: str) -> Term:
    m = _COUNT.match(text)
    if m:
        kind = m.group(2).lower().rstrip("s")
        kind = "legs" if kind == "leg" else kind
        return kind, int(m.group(1) or 1)
    count = 1
    m = _TIMES.match(text)
    if m:
        count, text = int(m.group(1)), m.group(2).strip()
    name = _names(side).get(text.lower())
    if name is None:
        raise LogicError(f"{where}: unknown name \"{text}\" (not a part, booster or upgrade of this side)")
    return name, count


def parse(text: str) -> Dict[Tuple[int, int], MissionLogic]:
    missions: Dict[Tuple[int, int], MissionLogic] = {}
    current = None
    side = 0
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        where = f"logic.txt line {number}"
        m = _HEADER.match(line)
        if m:
            side = 0 if m.group(1).lower() == "red star" else 1
            mission = int(m.group(2))
            if not 1 <= mission <= data.MISSIONS:
                raise LogicError(f"{where}: there is no mission {mission}")
            current = missions.setdefault((side, mission), MissionLogic())
            continue
        if current is None:
            raise LogicError(f"{where}: write a mission header (== Red Star Mission 1 ==) first")
        key, sep, value = line.partition(":")
        key, value = key.strip().lower(), value.strip()
        if not sep:
            raise LogicError(f"{where}: expected \"need:\", \"recommended:\" or \"base:\"")
        if key == "base":
            if value.lower() not in ("yes", "no"):
                raise LogicError(f"{where}: base is yes or no")
            current.base = value.lower() == "yes"
        elif key in ("need", "recommended"):
            requirement = [[_term(t.strip(), side, where) for t in option.split("&")] for option in value.split("|")]
            getattr(current, key).append(requirement)
        else:
            raise LogicError(f"{where}: unknown key \"{key}\"")
    return missions


def load() -> Dict[Tuple[int, int], MissionLogic]:
    raw = pkgutil.get_data(__name__.rpartition(".")[0], "logic.txt")
    return parse(raw.decode("utf-8"))


def met(state, player: int, requirement: Requirement, pools: Dict[str, List[str]]) -> bool:
    """pools: kind -> the item names of that kind the side can build"""
    for option in requirement:
        if all(state.has_from_list(pools[name], player, count) if name in KINDS else state.has(name, player, count)
               for name, count in option):
            return True
    return False


def kind_of(name: str) -> str:
    """the kind a part counts as: "damage weapon", "weapon" (no damage) or "legs"; "" for other items"""
    if name in data.LEGS:
        return "legs"
    if name in data.HEADS:
        return "weapon" if name in data.SUPPORT_HEADS else "damage weapon"
    return ""


def translate(requirement: Requirement, buildable: List[str]) -> Requirement:
    """A part the side can't build (Unholy Alliance gave it to the other side) asks for any part of its kind instead."""
    out: Requirement = []
    for option in requirement:
        terms: Dict[str, int] = {}
        for name, count in option:
            kind = kind_of(name)
            if kind and name not in buildable:
                name = kind
            terms[name] = terms.get(name, 0) + count
        out.append(list(terms.items()))
    return out
