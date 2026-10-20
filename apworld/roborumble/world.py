from typing import Any, Dict, List, Union

import settings

from BaseClasses import CollectionState, Item, ItemClassification, Location, LocationProgressType, Region, Tutorial
from worlds.AutoWorld import WebWorld, World

from . import data, logic
from .options import RoboRumbleOptions

SLOT_DATA_OPTIONS = (
    "side", "starting_side", "starting_parts", "starting_weapons", "goal", "open_missions", "final_mission_wins", "logic_strictness", "booster_builds",
    "combo_builds", "shop_checks", "spy_checks", "bonus_checks", "alien_artifacts", "extra_artifacts", "trap_percentage", "death_link",
    "unholy_alliance", "starting_robo_funds", "alien_booster_percentage",
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

    class DevMode(settings.Bool):
        """Allows the client's /dev commands: set off traps, win or lose the mission, set money, give yourself parts."""

    game_path: GamePath = GamePath("")
    renderer: Renderer = Renderer("auto")
    bridge_port: BridgePort = BridgePort(38480)
    dev_mode: Union[DevMode, bool] = False


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
        "Traps": set(data.TRAPS.values()),
        "Alien Boosters": set(data.ALIEN_BOOSTERS.values()),
        "Money": set(data.FILLER.values()),
        "Red Star Mission Access": {data.mission_access(0, n) for n in range(1, data.FINAL_MISSION + 1)},
        "Terraflux Mission Access": {data.mission_access(1, n) for n in range(1, data.FINAL_MISSION + 1)},
    }
    location_name_groups = {
        "Red Star Missions": set(_all_missions(0)),
        "Terraflux Missions": set(_all_missions(1)),
        "Builds": {data.build_head(n) for n in data.HEADS} | {data.build_legs(n) for n in data.LEGS},
        "Booster Builds": {data.build_booster(n) for n in data.BOOSTERS},
        "Combo Builds": {data.build_combo(h, l) for h in data.HEADS for l in data.LEGS},
        "Report Bonuses": {data.bonus(s, n, k) for s in (0, 1) for n in range(1, data.BONUS_MISSIONS + 1)
                           for k in range(len(data.BONUS_KINDS))},
        "Shop": set(data.shop_locations(0) + data.shop_locations(1)),
        "Spy Reports": {data.spy_hint(s, n) for s in (0, 1) for n in range(1, data.MISSIONS + 1)},
    }

    ut_can_gen_without_yaml = True

    sides: List[int]
    start_heads: List[str]
    start_legs: List[str]
    rows_heads: List[int]   # Unholy Alliance: builder slot (side * 8 + i) -> weapon index (identity when off)
    rows_legs: List[int]

    def side_heads(self, side: int) -> List[str]:
        """the weapons this side builds (Unholy Alliance can give it the other side's)"""
        return [data.HEADS[t] for t in self.rows_heads[side * 8:side * 8 + 8]]

    def side_legs(self, side: int) -> List[str]:
        return [data.LEGS[t] for t in self.rows_legs[side * 8:side * 8 + 8]]

    def side_attack_heads(self, side: int) -> List[str]:
        return [h for h in self.side_heads(side) if h not in data.SUPPORT_HEADS]

    def make_rows(self) -> None:
        heads, legs = list(range(16)), list(range(16))
        mode = self.options.unholy_alliance
        for row in (heads, legs):
            if mode == "switch":
                for i in range(8):
                    if self.random.random() < 0.5:
                        row[i], row[i + 8] = row[i + 8], row[i]
            elif mode == "full_shuffle":
                self.random.shuffle(row)
                row[:8], row[8:] = sorted(row[:8]), sorted(row[8:])
        self.rows_heads, self.rows_legs = heads, legs

    def pools(self, side: int) -> Dict[str, List[str]]:
        return {"weapon": self.side_heads(side), "damage weapon": self.side_attack_heads(side),
                "legs": self.side_legs(side)}

    def generate_early(self) -> None:
        passthrough: Dict[str, Any] = getattr(self.multiworld, "re_gen_passthrough", {}).get(self.game, {})
        for option_name in SLOT_DATA_OPTIONS:
            if option_name in passthrough:
                getattr(self.options, option_name).value = passthrough[option_name]
        if passthrough and "rows_heads" in passthrough:
            self.rows_heads, self.rows_legs = list(passthrough["rows_heads"]), list(passthrough["rows_legs"])
        else:
            self.make_rows()

        # logic.txt names the original parts: a part this side doesn't get asks for one of its kind instead
        self.mission_logic = logic.load()
        for (side, _), rules in self.mission_logic.items():
            buildable = self.side_heads(side) + self.side_legs(side)
            rules.need = [logic.translate(r, buildable) for r in rules.need]
            rules.recommended = [logic.translate(r, buildable) for r in rules.recommended]
        # what logic asks for has to be progression (upgrades and inventions are only useful otherwise)
        relaxed = self.options.logic_strictness == "relaxed"
        self.logic_items = {name for rules in self.mission_logic.values()
                            for need in (rules.need if relaxed else rules.need + rules.recommended)
                            for option in need for name, _ in option if name not in logic.KINDS}

        self.sides = [0, 1] if self.options.side == "both" else [self.options.side.value]
        # the side whose first mission is open; with both sides the other one's needs its access item
        self.start_side = self.options.starting_side.value if len(self.sides) == 2 else self.sides[0]
        if passthrough and "start_heads" in passthrough:
            self.start_heads = list(passthrough["start_heads"])
            self.start_legs = list(passthrough["start_legs"])
            return
        self.start_heads, self.start_legs = [], []
        random_weapons = self.options.starting_parts in ("random_weapons", "random_all")
        for side in self.sides:
            # the original first mission's builder slots (with Unholy Alliance: whatever is in those slots now)
            heads = [self.side_heads(side)[i] for i, c in enumerate(data.VANILLA_HEADS[side][0]) if c == "+"]
            legs = [self.side_legs(side)[i] for i, c in enumerate(data.VANILLA_LEGS[side][0]) if c == "+"]
            if random_weapons or not any(h in self.side_attack_heads(side) for h in heads):
                # one weapon that does damage, the rest anything
                first = self.random.choice(self.side_attack_heads(side))
                rest = [h for h in self.side_heads(side) if h != first]
                count = self.options.starting_weapons.value if random_weapons else len(heads)
                heads = [first] + self.random.sample(rest, count - 1)
            if self.options.starting_parts in ("random_legs", "random_all"):
                legs = [self.random.choice(self.side_legs(side))]
            self.start_heads += heads
            self.start_legs += legs

    # ---------------------------------------------------------------- items
    def create_item(self, name: str) -> RoboRumbleItem:
        if name in data.FILLER.values():
            classification = ItemClassification.filler
        elif name in data.ALIEN_BOOSTERS.values():
            classification = ItemClassification.useful
        elif name == data.ROBO_FUNDS:
            classification = (ItemClassification.progression if name in getattr(self, "logic_items", ())
                              else ItemClassification.useful)
        elif name in data.TRAPS.values():
            classification = ItemClassification.trap
        elif (name in data.UPGRADES or name in data.INVENTIONS) and name not in getattr(self, "logic_items", ()):
            classification = ItemClassification.useful
        elif name == data.ARTIFACT:
            classification = ItemClassification.progression_skip_balancing
        else:
            classification = ItemClassification.progression
        return RoboRumbleItem(name, classification, self.item_name_to_id[name], self.player)

    def get_filler_item_name(self) -> str:
        return self.random.choices(list(data.FILLER.values()), weights=data.MONEY_WEIGHTS)[0]

    def create_items(self) -> None:
        open_missions = self.options.open_missions.value
        for name in self.start_heads + self.start_legs:
            self.multiworld.push_precollected(self.create_item(name))

        pool: List[RoboRumbleItem] = []
        for side in self.sides:
            if side != self.start_side:
                pool.append(self.create_item(data.mission_access(side, 1)))
            if self.final_access_item():
                pool.append(self.create_item(data.mission_access(side, data.FINAL_MISSION)))
            for number in range(2, data.FINAL_MISSION):
                item = self.create_item(data.mission_access(side, number))
                if side == self.start_side and number <= open_missions:
                    self.multiworld.push_precollected(item)
                else:
                    pool.append(item)
            for name in self.side_heads(side) + self.side_legs(side):
                if name not in self.start_heads and name not in self.start_legs:
                    pool.append(self.create_item(name))
        for name in data.BOOSTERS + data.UPGRADES + data.INVENTIONS:
            pool.append(self.create_item(name))
        pool += [self.create_item(data.ROBO_FUNDS) for _ in range(self.options.starting_robo_funds.value)]
        if self.artifact_goal():
            artifacts = self.options.alien_artifacts.value + self.options.extra_artifacts.value
            pool += [self.create_item(data.ARTIFACT) for _ in range(artifacts)]

        missing = len(self.multiworld.get_unfilled_locations(self.player)) - len(pool)
        if missing < 0:
            # few checks turned on: items that are only useful are given at the start instead
            for item in [i for i in pool if i.classification == ItemClassification.useful][:-missing]:
                pool.remove(item)
                self.multiworld.push_precollected(item)
                missing += 1
        if missing < 0:
            raise Exception(f"RoboRumble ({self.player_name}): {-missing} more items than checks; "
                            "turn on shop_checks, booster_builds, spy_checks or combo_builds")
        weights = [self.options.oil_trap_weight.value, self.options.freeze_trap_weight.value,
                   self.options.disruptor_trap_weight.value, self.options.mine_trap_weight.value,
                   self.options.nuke_trap_weight.value]
        traps = round(max(missing, 0) * self.options.trap_percentage.value / 100) if sum(weights) else 0
        if traps:
            names = self.random.choices(list(data.TRAPS.values()), weights=weights, k=traps)
            pool += [self.create_item(name) for name in names]
        rest = missing - traps
        aliens = round(max(rest, 0) * self.options.alien_booster_percentage.value / 100)
        pool += [self.create_item(self.random.choice(list(data.ALIEN_BOOSTERS.values()))) for _ in range(aliens)]
        pool += [self.create_item(self.get_filler_item_name()) for _ in range(rest - aliens)]
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
            names += [data.build_head(n) for n in self.side_heads(side)]
            names += [data.build_legs(n) for n in self.side_legs(side)]
            if self.options.combo_builds:
                names += [data.build_combo(h, l) for h in self.side_heads(side) for l in self.side_legs(side)]
            if self.options.spy_checks:
                names += [data.spy_hint(side, n) for n in range(1, data.MISSIONS + 1)]
            if self.options.bonus_checks:
                names += [data.bonus(side, n, k) for n in range(1, data.BONUS_MISSIONS + 1)
                          for k in range(len(data.BONUS_KINDS))]
        if self.options.booster_builds:
            names += [data.build_booster(n) for n in data.BOOSTERS]
        if self.options.shop_checks:
            for side in self.sides:
                names += data.shop_locations(side)
        campaign.add_locations({name: self.location_name_to_id[name] for name in names}, RoboRumbleLocation)
        if self.options.bonus_checks:
            # which robots beat the records isn't known yet: no progression there
            for side in self.sides:
                for n in range(1, data.BONUS_MISSIONS + 1):
                    for k in range(len(data.BONUS_KINDS)):
                        self.get_location(data.bonus(side, n, k)).progress_type = LocationProgressType.EXCLUDED

        if self.options.goal == "alien_artifacts_only":
            need = self.options.alien_artifacts.value
            self.multiworld.completion_condition[self.player] = (
                lambda state: state.has(data.ARTIFACT, self.player, need))
            return
        for side in self.sides:
            goal = RoboRumbleLocation(self.player, data.final_goal(side), None, campaign)
            goal.place_locked_item(RoboRumbleItem(VICTORY, ItemClassification.progression, None, self.player))
            campaign.locations.append(goal)
        sides = len(self.sides)
        self.multiworld.completion_condition[self.player] = lambda state: state.has(VICTORY, self.player, sides)

    def final_access_item(self) -> bool:
        """The final mission opens with its own access item (otherwise wins or artifacts open it)."""
        return self.options.goal in ("final_mission_only", "alien_artifacts_only")

    def artifact_goal(self) -> bool:
        return self.options.goal in ("alien_artifacts_only", "alien_artifacts_and_final_mission")

    # ---------------------------------------------------------------- logic
    def has_parts(self, state: CollectionState, side: int, mission: int) -> bool:
        """What logic.txt asks for that mission ("recommended" lines only with normal strictness)."""
        rules = self.mission_logic.get((side, mission))
        if rules is None:
            return True
        needs = rules.need if self.options.logic_strictness == "relaxed" else rules.need + rules.recommended
        pools = self.pools(side)
        return all(logic.met(state, self.player, need, pools) for need in needs)

    def has_base(self, side: int, mission: int) -> bool:
        rules = self.mission_logic.get((side, mission))
        return rules is None or rules.base

    def mission_open(self, state: CollectionState, side: int, mission: int) -> bool:
        if mission == data.FINAL_MISSION and self.options.goal == "set_missions_and_final_mission":
            wins = sum(1 for n in range(1, data.FINAL_MISSION) if self.can_play(state, side, n))
            return wins >= self.options.final_mission_wins.value
        if mission == data.FINAL_MISSION and self.options.goal == "alien_artifacts_and_final_mission":
            return state.has(data.ARTIFACT, self.player, self.options.alien_artifacts.value)
        if mission == 1 and side == self.start_side:
            return True
        return state.has(data.mission_access(side, mission), self.player)

    def can_play(self, state: CollectionState, side: int, mission: int) -> bool:
        return self.mission_open(state, side, mission) and self.has_parts(state, side, mission)

    def side_reachable(self, state: CollectionState, side: int) -> bool:
        """Some mission of that side with a factory is open (builds happen in missions)."""
        return any(self.mission_open(state, side, n) for n in range(1, data.FINAL_MISSION) if self.has_base(side, n))

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
            if self.options.goal != "alien_artifacts_only":
                self.get_location(data.final_goal(side)).access_rule = (
                    lambda state, s=side: self.can_play(state, s, data.FINAL_MISSION))
            if self.options.spy_checks:
                for number in range(1, data.MISSIONS + 1):
                    self.get_location(data.spy_hint(side, number)).access_rule = (
                        lambda state, s=side, n=number: self.mission_open(state, s, n))
            if self.options.bonus_checks:
                for number in range(1, data.BONUS_MISSIONS + 1):
                    for kind in range(len(data.BONUS_KINDS)):
                        self.get_location(data.bonus(side, number, kind)).access_rule = (
                            lambda state, s=side, n=number: self.can_play(state, s, n))
            for name in self.side_heads(side):
                self.get_location(data.build_head(name)).access_rule = (
                    lambda state, n=name, s=side: state.has(n, player) and self.side_reachable(state, s))
            for name in self.side_legs(side):
                self.get_location(data.build_legs(name)).access_rule = (
                    lambda state, n=name, s=side: state.has(n, player) and self.side_reachable(state, s))
            if self.options.combo_builds:
                for head in self.side_heads(side):
                    for legs in self.side_legs(side):
                        self.get_location(data.build_combo(head, legs)).access_rule = (
                            lambda state, h=head, l=legs, s=side: state.has_all((h, l), player)
                            and self.side_reachable(state, s))
        if self.options.booster_builds:
            for name in data.BOOSTERS:
                self.get_location(data.build_booster(name)).access_rule = lambda state, n=name: state.has(n, player)
        # The shop's goods are in logic as soon as a side's briefings can be reached: haggling can get any of them
        # for free.
        if self.options.shop_checks:
            for side in self.sides:
                for name in data.shop_locations(side):
                    self.get_location(name).access_rule = lambda state, s=side: self.side_reachable(state, s)

    def fill_slot_data(self) -> Dict[str, Any]:
        slot = {name: getattr(self.options, name).value for name in SLOT_DATA_OPTIONS}
        slot["start_side"] = self.start_side
        slot["start_heads"] = self.start_heads
        slot["start_legs"] = self.start_legs
        slot["rows_heads"] = self.rows_heads
        slot["rows_legs"] = self.rows_legs
        return slot

    @staticmethod
    def interpret_slot_data(slot_data: Dict[str, Any]) -> Dict[str, Any]:
        return slot_data
