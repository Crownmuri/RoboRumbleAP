from typing import Any, Dict, List

import settings

from BaseClasses import CollectionState, Item, ItemClassification, Location, Region, Tutorial
from worlds.AutoWorld import WebWorld, World

from . import data
from .options import RoboRumbleOptions

SLOT_DATA_OPTIONS = ("side", "starting_parts", "open_missions", "logic_strictness", "booster_builds", "combo_builds", "shop_checks", "spy_checks",
                     "alien_artifacts", "extra_artifacts", "death_link")

VICTORY = "Victory"


class RoboRumbleItem(Item):
    game = data.GAME_NAME


class RoboRumbleLocation(Location):
    game = data.GAME_NAME


class RoboRumbleSettings(settings.Group):
    class BridgePort(int):
        """Local port the client listens on for the game patch ([ap] port in the game's rrpatch.ini)."""

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

    item_name_to_id = data.item_table()
    location_name_to_id = data.location_table()

    item_name_groups = {
        "Weapons": set(data.HEADS),
        "Legs": set(data.LEGS),
        "Boosters": set(data.BOOSTERS),
        "Base Upgrades": set(data.UPGRADES),
        "Inventions": set(data.INVENTIONS),
        "Mission Access": {data.mission_access(n) for n in range(2, data.MISSIONS + 1)},
    }
    location_name_groups = {
        "Missions": {data.mission_complete(n) for n in range(1, data.MISSIONS + 1)},
        "Builds": {data.build_head(n) for n in data.HEADS} | {data.build_legs(n) for n in data.LEGS},
        "Booster Builds": {data.build_booster(n) for n in data.BOOSTERS},
        "Combo Builds": set(data.side_combos(0) + data.side_combos(1)),
        "Shop": {data.shop_booster(n) for n in data.BOOSTERS} | {data.shop_upgrade(n) for n in data.UPGRADES}
        | {data.shop_invention(n) for n in data.INVENTIONS},
        "Spy Reports": {data.spy_hint(n) for n in range(1, data.MISSIONS + 1)},
    }

    ut_can_gen_without_yaml = True

    side: int
    start_heads: List[str]
    start_legs: List[str]

    def generate_early(self) -> None:
        passthrough: Dict[str, Any] = getattr(self.multiworld, "re_gen_passthrough", {}).get(self.game, {})
        for option_name in SLOT_DATA_OPTIONS:
            if option_name in passthrough:
                getattr(self.options, option_name).value = passthrough[option_name]

        self.side = self.options.side.value
        if passthrough and "start_heads" in passthrough:
            self.start_heads = list(passthrough["start_heads"])
            self.start_legs = list(passthrough["start_legs"])
        elif self.options.starting_parts == "minimal":
            self.start_heads = [self.random.choice(data.side_heads(self.side))]
            self.start_legs = [data.side_legs(self.side)[0]]
        else:
            self.start_heads, self.start_legs = data.starting_parts(self.side)

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

    def open_mission_numbers(self) -> range:
        return range(2, self.options.open_missions.value + 1)

    def create_items(self) -> None:
        for name in self.start_heads + self.start_legs:
            self.multiworld.push_precollected(self.create_item(name))
        for number in self.open_mission_numbers():
            self.multiworld.push_precollected(self.create_item(data.mission_access(number)))

        pool: List[RoboRumbleItem] = []
        for name in data.side_heads(self.side) + data.side_legs(self.side):
            if name not in self.start_heads and name not in self.start_legs:
                pool.append(self.create_item(name))
        for name in data.BOOSTERS:
            pool.append(self.create_item(name))
        for number in range(self.options.open_missions.value + 1, data.MISSIONS + 1):
            pool.append(self.create_item(data.mission_access(number)))
        for name in data.UPGRADES + data.INVENTIONS:
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

        names = [data.mission_complete(n) for n in range(1, data.MISSIONS + 1)]
        names += [data.build_head(n) for n in data.side_heads(self.side)]
        names += [data.build_legs(n) for n in data.side_legs(self.side)]
        if self.options.booster_builds:
            names += [data.build_booster(n) for n in data.BOOSTERS]
        if self.options.combo_builds:
            names += data.side_combos(self.side)
        if self.options.shop_checks:
            names += [data.shop_booster(n) for n in data.BOOSTERS]
            names += [data.shop_upgrade(n) for n in data.UPGRADES]
            names += [data.shop_invention(n) for n in data.INVENTIONS]
        if self.options.spy_checks:
            names += [data.spy_hint(n) for n in range(1, data.MISSIONS + 1)]
        campaign.add_locations({name: self.location_name_to_id[name] for name in names}, RoboRumbleLocation)

        goal = RoboRumbleLocation(self.player, "Defeat the Aliens", None, campaign)
        goal.place_locked_item(RoboRumbleItem(VICTORY, ItemClassification.progression, None, self.player))
        campaign.locations.append(goal)
        self.multiworld.completion_condition[self.player] = lambda state: state.has(VICTORY, self.player)

    def can_play(self, state: CollectionState, mission: int) -> bool:
        """Logic is loose for now: the mission's access item and about as many parts as the original offers."""
        if mission > 1 and not state.has(data.mission_access(mission), self.player):
            return False
        heads, legs = data.vanilla_counts(self.side, mission)
        if self.options.logic_strictness == "relaxed":
            heads, legs = max(1, heads // 2), max(1, legs // 2)
        else:
            heads, legs = max(1, heads - 1), max(1, legs - 1)
        return (state.has_from_list(data.side_heads(self.side), self.player, heads)
                and state.has_from_list(data.side_legs(self.side), self.player, legs))

    def missions_open(self, state: CollectionState, count: int) -> bool:
        """At least `count` missions besides the first are unlocked."""
        return state.has_group("Mission Access", self.player, count)

    def set_rules(self) -> None:
        player = self.player
        for number in range(1, data.MISSIONS + 1):
            self.get_location(data.mission_complete(number)).access_rule = (
                lambda state, n=number: self.can_play(state, n))
        artifacts = self.options.alien_artifacts.value
        self.get_location("Defeat the Aliens").access_rule = (
            lambda state: self.can_play(state, data.MISSIONS) and state.has(data.ARTIFACT, player, artifacts))
        if self.options.shop_checks:
            # Shop money comes from won missions; loosely: the dearer goods after a few missions are open
            for name in data.BOOSTERS:
                self.get_location(data.shop_booster(name)).access_rule = lambda state: self.missions_open(state, 1)
            for name in data.UPGRADES:
                self.get_location(data.shop_upgrade(name)).access_rule = lambda state: self.missions_open(state, 3)
            for name in data.INVENTIONS:
                self.get_location(data.shop_invention(name)).access_rule = lambda state: self.missions_open(state, 5)
        if self.options.spy_checks:
            for number in range(2, data.MISSIONS + 1):
                self.get_location(data.spy_hint(number)).access_rule = (
                    lambda state, n=number: state.has(data.mission_access(n), player))
        for name in data.side_heads(self.side):
            self.get_location(data.build_head(name)).access_rule = lambda state, n=name: state.has(n, player)
        for name in data.side_legs(self.side):
            self.get_location(data.build_legs(name)).access_rule = lambda state, n=name: state.has(n, player)
        if self.options.booster_builds:
            for name in data.BOOSTERS:
                self.get_location(data.build_booster(name)).access_rule = lambda state, n=name: state.has(n, player)

        if self.options.combo_builds:
            for head in data.side_heads(self.side):
                for legs in data.side_legs(self.side):
                    self.get_location(data.build_combo(head, legs)).access_rule = (
                        lambda state, h=head, l=legs: state.has_all((h, l), player))

    def fill_slot_data(self) -> Dict[str, Any]:
        slot = {name: getattr(self.options, name).value for name in SLOT_DATA_OPTIONS}
        slot["start_heads"] = self.start_heads
        slot["start_legs"] = self.start_legs
        return slot

    @staticmethod
    def interpret_slot_data(slot_data: Dict[str, Any]) -> Dict[str, Any]:
        return slot_data
