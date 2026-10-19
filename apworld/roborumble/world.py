from typing import Any, Dict, List

import settings

from BaseClasses import CollectionState, Item, ItemClassification, Location, Region, Tutorial
from worlds.AutoWorld import WebWorld, World

from . import data
from .options import RoboRumbleOptions

SLOT_DATA_OPTIONS = (
    "side", "starting_side", "starting_parts", "open_missions", "final_mission_wins", "logic_strictness", "booster_builds",
    "combo_builds", "shop_checks", "spy_checks", "alien_artifacts", "extra_artifacts", "death_link",
)

VICTORY = "Victory"


class RoboRumbleItem(Item):
    game = data.GAME_NAME


class RoboRumbleLocation(Location):
    game = data.GAME_NAME


class RoboRumbleSettings(settings.Group):
    class GamePath(str):
        """RoboRumble install folder (the one with DATA in it). Leave empty to find the GOG or Steam install."""

    class Renderer(str):
        """Renderer /run starts: auto (Glide if a Glide wrapper is there, else software), glide, software, direct3d."""

    class BridgePort(int):
        """Local port the client listens on for the game patch ([ap] port in the game's rrpatch.ini)."""

    game_path: GamePath = GamePath("")
    renderer: Renderer = Renderer("auto")
    bridge_port: BridgePort = BridgePort(38480)


class RoboRumbleWebWorld(WebWorld):
    theme = "partyTime"
    tutorials: List[Tutorial] = [
        Tutorial(
            "Multiworld Setup Guide",
            "A guide to setting up RoboRumble for Archipelago",
            "English",
            "setup_en.md",
            "setup/en",
            ["Crownmuri"],
        )
    ]


def _all_missions(side: int) -> List[str]:
    return [data.mission_complete(side, n) for n in range(1, data.MISSIONS + 1)]


class RoboRumbleWorld(World):
    """
    RoboRumble is a 1998 real-time strategy game: build robots from weapons, legs and alien boosters, then send
    them out to destroy the enemy base. Missions are unlocked by items and can be played in any order.
    """

    game = data.GAME_NAME
    options_dataclass = RoboRumbleOptions
    options: RoboRumbleOptions
    web = RoboRumbleWebWorld()
    settings: RoboRumbleSettings
    settings_key = "roborumble_options"

    item_name_to_id = data.item_table()
    location_name_to_id = data.location_table()

    item_name_groups = {
        "Weapons": set(data.HEADS),
        "Legs": set(data.LEGS),
        "Boosters": set(data.BOOSTERS),
        "Base Upgrades": set(data.UPGRADES),
        "Inventions": set(data.INVENTIONS),
        "Red Star Mission Access": {data.mission_access(0, n) for n in range(1, data.FINAL_MISSION)},
        "Terraflux Mission Access": {data.mission_access(1, n) for n in range(1, data.FINAL_MISSION)},
    }
    location_name_groups = {
        "Red Star Missions": set(_all_missions(0)),
        "Terraflux Missions": set(_all_missions(1)),
        "Builds": {data.build_head(n) for n in data.HEADS} | {data.build_legs(n) for n in data.LEGS},
        "Booster Builds": {data.build_booster(n) for n in data.BOOSTERS},
        "Combo Builds": set(data.side_combos(0) + data.side_combos(1)),
        "Shop": {data.shop_booster(n) for n in data.BOOSTERS} | {data.shop_upgrade(n) for n in data.UPGRADES}
        | {data.shop_invention(n) for n in data.INVENTIONS},
        "Spy Reports": {data.spy_hint(s, n) for s in (0, 1) for n in range(1, data.MISSIONS + 1)},
    }

    ut_can_gen_without_yaml = True

    sides: List[int]
    start_heads: List[str]
    start_legs: List[str]

    def generate_early(self) -> None:
        passthrough: Dict[str, Any] = getattr(self.multiworld, "re_gen_passthrough", {}).get(self.game, {})
        for option_name in SLOT_DATA_OPTIONS:
            if option_name in passthrough:
                getattr(self.options, option_name).value = passthrough[option_name]

        self.sides = [0, 1] if self.options.side == "both" else [self.options.side.value]
        # the side whose first mission is open; with both sides the other one's needs its access item
        self.start_side = self.options.starting_side.value if len(self.sides) == 2 else self.sides[0]
        if passthrough and "start_heads" in passthrough:
            self.start_heads = list(passthrough["start_heads"])
            self.start_legs = list(passthrough["start_legs"])
            return
        self.start_heads, self.start_legs = [], []
        for side in self.sides:
            if self.options.starting_parts == "minimal":
                self.start_heads.append(self.random.choice(data.side_heads(side)))
                self.start_legs.append(data.side_legs(side)[0])
            else:
                heads, legs = data.starting_parts(side)
                self.start_heads += heads
                self.start_legs += legs

    # ---------------------------------------------------------------- items
    def create_item(self, name: str) -> RoboRumbleItem:
        if name in data.FILLER.values():
            classification = ItemClassification.filler
        elif name in data.UPGRADES or name in data.INVENTIONS:
            classification = ItemClassification.useful
        elif name == data.ARTIFACT:
            classification = ItemClassification.progression_skip_balancing
        else:
            classification = ItemClassification.progression
        return RoboRumbleItem(name, classification, self.item_name_to_id[name], self.player)

    def get_filler_item_name(self) -> str:
        return self.random.choice(list(data.FILLER.values()))

    def create_items(self) -> None:
        open_missions = self.options.open_missions.value
        for name in self.start_heads + self.start_legs:
            self.multiworld.push_precollected(self.create_item(name))

        pool: List[RoboRumbleItem] = []
        for side in self.sides:
            if side != self.start_side:
                pool.append(self.create_item(data.mission_access(side, 1)))
            for number in range(2, data.FINAL_MISSION):
                item = self.create_item(data.mission_access(side, number))
                if side == self.start_side and number <= open_missions:
                    self.multiworld.push_precollected(item)
                else:
                    pool.append(item)
            for name in data.side_heads(side) + data.side_legs(side):
                if name not in self.start_heads and name not in self.start_legs:
                    pool.append(self.create_item(name))
        for name in data.BOOSTERS + data.UPGRADES + data.INVENTIONS:
            pool.append(self.create_item(name))
        if self.options.alien_artifacts:
            artifacts = self.options.alien_artifacts.value + self.options.extra_artifacts.value
            pool += [self.create_item(data.ARTIFACT) for _ in range(artifacts)]

        missing = len(self.multiworld.get_unfilled_locations(self.player)) - len(pool)
        pool += [self.create_item(self.get_filler_item_name()) for _ in range(missing)]
        self.multiworld.itempool += pool

    # ---------------------------------------------------------------- locations
    def create_regions(self) -> None:
        menu = Region("Menu", self.player, self.multiworld)
        campaign = Region("Campaign", self.player, self.multiworld)
        self.multiworld.regions += [menu, campaign]
        menu.connect(campaign)

        names: List[str] = []
        for side in self.sides:
            names += _all_missions(side)
            names += [data.build_head(n) for n in data.side_heads(side)]
            names += [data.build_legs(n) for n in data.side_legs(side)]
            if self.options.combo_builds:
                names += data.side_combos(side)
            if self.options.spy_checks:
                names += [data.spy_hint(side, n) for n in range(1, data.MISSIONS + 1)]
        if self.options.booster_builds:
            names += [data.build_booster(n) for n in data.BOOSTERS]
        if self.options.shop_checks:
            names += [data.shop_booster(n) for n in data.BOOSTERS]
            names += [data.shop_upgrade(n) for n in data.UPGRADES]
            names += [data.shop_invention(n) for n in data.INVENTIONS]
        campaign.add_locations({name: self.location_name_to_id[name] for name in names}, RoboRumbleLocation)

        for side in self.sides:
            goal = RoboRumbleLocation(self.player, data.final_goal(side), None, campaign)
            goal.place_locked_item(RoboRumbleItem(VICTORY, ItemClassification.progression, None, self.player))
            campaign.locations.append(goal)
        sides = len(self.sides)
        self.multiworld.completion_condition[self.player] = lambda state: state.has(VICTORY, self.player, sides)

    # ---------------------------------------------------------------- logic
    def has_parts(self, state: CollectionState, side: int, mission: int) -> bool:
        """Loose for now: about as many weapons and legs as the original game offers in that mission."""
        heads, legs = data.vanilla_counts(side, mission)
        if self.options.logic_strictness == "relaxed":
            heads, legs = max(1, heads // 2), max(1, legs // 2)
        else:
            heads, legs = max(1, heads - 1), max(1, legs - 1)
        return (state.has_from_list(data.side_heads(side), self.player, heads)
                and state.has_from_list(data.side_legs(side), self.player, legs))

    def mission_open(self, state: CollectionState, side: int, mission: int) -> bool:
        if mission == data.FINAL_MISSION:
            wins = sum(1 for n in range(1, data.FINAL_MISSION) if self.can_play(state, side, n))
            return wins >= self.options.final_mission_wins.value
        if mission == 1 and side == self.start_side:
            return True
        return state.has(data.mission_access(side, mission), self.player)

    def can_play(self, state: CollectionState, side: int, mission: int) -> bool:
        return self.mission_open(state, side, mission) and self.has_parts(state, side, mission)

    def side_reachable(self, state: CollectionState, side: int) -> bool:
        """Some mission of that side can be played (builds happen in missions)."""
        return any(self.mission_open(state, side, n) for n in range(1, data.FINAL_MISSION))

    def missions_open(self, state: CollectionState, count: int) -> bool:
        """At least `count` missions besides the first are unlocked, on any side."""
        found = sum(state.count_group(f"{data.SIDES[side]} Mission Access", self.player) for side in self.sides)
        return found >= count

    def set_rules(self) -> None:
        player = self.player
        artifacts = self.options.alien_artifacts.value
        for side in self.sides:
            for number in range(1, data.MISSIONS + 1):
                self.get_location(data.mission_complete(side, number)).access_rule = (
                    lambda state, s=side, n=number: self.can_play(state, s, n))
            self.get_location(data.final_goal(side)).access_rule = (
                lambda state, s=side: self.can_play(state, s, data.FINAL_MISSION)
                and state.has(data.ARTIFACT, player, artifacts))
            if self.options.spy_checks:
                for number in range(1, data.MISSIONS + 1):
                    self.get_location(data.spy_hint(side, number)).access_rule = (
                        lambda state, s=side, n=number: self.mission_open(state, s, n))
            for name in data.side_heads(side):
                self.get_location(data.build_head(name)).access_rule = (
                    lambda state, n=name, s=side: state.has(n, player) and self.side_reachable(state, s))
            for name in data.side_legs(side):
                self.get_location(data.build_legs(name)).access_rule = (
                    lambda state, n=name, s=side: state.has(n, player) and self.side_reachable(state, s))
            if self.options.combo_builds:
                for head in data.side_heads(side):
                    for legs in data.side_legs(side):
                        self.get_location(data.build_combo(head, legs)).access_rule = (
                            lambda state, h=head, l=legs, s=side: state.has_all((h, l), player)
                            and self.side_reachable(state, s))
        if self.options.booster_builds:
            for name in data.BOOSTERS:
                self.get_location(data.build_booster(name)).access_rule = lambda state, n=name: state.has(n, player)
        if self.options.shop_checks:
            # Shop money comes from won missions; loosely: the dearer goods after a few missions are open
            for name in data.BOOSTERS:
                self.get_location(data.shop_booster(name)).access_rule = lambda state: self.missions_open(state, 1)
            for name in data.UPGRADES:
                self.get_location(data.shop_upgrade(name)).access_rule = lambda state: self.missions_open(state, 3)
            for name in data.INVENTIONS:
                self.get_location(data.shop_invention(name)).access_rule = lambda state: self.missions_open(state, 5)

    def fill_slot_data(self) -> Dict[str, Any]:
        slot = {name: getattr(self.options, name).value for name in SLOT_DATA_OPTIONS}
        slot["start_side"] = self.start_side
        slot["start_heads"] = self.start_heads
        slot["start_legs"] = self.start_legs
        return slot

    @staticmethod
    def interpret_slot_data(slot_data: Dict[str, Any]) -> Dict[str, Any]:
        return slot_data
