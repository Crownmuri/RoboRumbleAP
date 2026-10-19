"""
Reads logic.txt: per mission, what logic expects you to have before it sends you in (format: see logic.txt).

A requirement is a list of options (written "A | B"), an option a list of terms that are all needed ("A & B").
A term is an item name or a count of a kind of part ("2 damage weapons").
"""
import pkgutil
import re
from dataclasses import dataclass, field
from typing import Dict, List, Tuple, Union

from . import data

Term = Union[str, Tuple[str, int]]   # item name, or (kind, count) with kind "weapon", "damage weapon", "legs"
Requirement = List[List[Term]]


@dataclass
class MissionLogic:
    base: bool = True
    need: List[Requirement] = field(default_factory=list)
    recommended: List[Requirement] = field(default_factory=list)


class LogicError(Exception):
    pass


_HEADER = re.compile(r"^==\s*(Red Star|Terraflux)\s+Mission\s+(\d+)\s*==$", re.IGNORECASE)
_COUNT = re.compile(r"^(?:(\d+)\s+|any\s+)?(damage weapons?|weapons?|legs)$", re.IGNORECASE)


def _names(side: int) -> Dict[str, str]:
    """lower-case name -> item name, for what can be written in a mission of that side"""
    names = {n.lower(): n for n in data.side_heads(side) + data.side_legs(side)}
    for n in data.BOOSTERS + data.UPGRADES + data.INVENTIONS:
        names[n.lower()] = n
    names["2-legs"] = data.side_legs(side)[0]
    return names


def _term(text: str, side: int, where: str) -> Term:
    m = _COUNT.match(text)
    if m:
        kind = m.group(2).lower().rstrip("s")
        kind = "legs" if kind == "leg" else kind
        return kind, int(m.group(1) or 1)
    name = _names(side).get(text.lower())
    if name is None:
        raise LogicError(f"{where}: unknown name \"{text}\" (not a part, booster or upgrade of this side)")
    return name


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


def _pool(side: int, kind: str) -> List[str]:
    if kind == "weapon":
        return data.side_heads(side)
    if kind == "damage weapon":
        return data.side_attack_heads(side)
    return data.side_legs(side)


def met(state, player: int, side: int, requirement: Requirement) -> bool:
    for option in requirement:
        if all(state.has(t, player) if isinstance(t, str) else state.has_from_list(_pool(side, t[0]), player, t[1])
               for t in option):
            return True
    return False
