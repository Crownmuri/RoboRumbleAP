from dataclasses import dataclass

from Options import Choice, DeathLink, DefaultOnToggle, PerGameCommonOptions, Range, Toggle


class Side(Choice):
    """
    The campaign to play. Each side has its own 16 missions and its own eight weapons and eight legs.
    Red Star: Chain Gun, Rocket Launcher, ... Nuke. Terraflux: Laser, Pulse Gun, ... Infector.
    both: both campaigns; the goal is the final mission of each side.
    """
    display_name = "Side"
    option_red_star = 0
    option_terraflux = 1
    option_both = 2
    default = 0


class StartingSide(Choice):
    """
    With side: both, the campaign whose first mission is open from the start. The other side's first mission
    needs its Mission 1 Access item.
    """
    display_name = "Starting Side"
    option_red_star = 0
    option_terraflux = 1
    default = "random"


class StartingParts(Choice):
    """
    What you can build at the start.
    normal: the weapons and legs the first mission gives in the original game.
    random_weapons: random weapons (as many as starting_weapons, at least one of them doing damage) and the side's
    basic legs.
    random_legs: the original first weapons and random legs.
    random_all: random weapons and random legs.
    """
    display_name = "Starting Parts"
    option_normal = 0
    option_random_weapons = 1
    option_random_legs = 2
    option_random_all = 3
    alias_vanilla = 0
    alias_minimal = 1
    default = 1


class StartingWeapons(Range):
    """With random starting weapons, how many weapons of each side you start with."""
    display_name = "Starting Weapons"
    range_start = 1
    range_end = 4
    default = 2


class Goal(Choice):
    """
    final_mission_only: win the final mission (16). Its access item is shuffled into the pool like the others.
    set_missions_and_final_mission: win the final mission; it opens after final_mission_wins other missions are won.
    alien_artifacts_only: collect the Alien Artifacts (alien_artifacts); that alone is the goal.
    alien_artifacts_and_final_mission: win the final mission; it opens once you have the Alien Artifacts.
    With side: both, the final mission goals count for both campaigns.
    """
    display_name = "Goal"
    option_final_mission_only = 0
    option_set_missions_and_final_mission = 1
    option_alien_artifacts_only = 2
    option_alien_artifacts_and_final_mission = 3
    default = 1


class OpenMissions(Range):
    """
    How many missions of the starting side are open from the start (its mission 1 always is). Every other mission up
    to 15 needs its Mission Access item; unlocked missions can be played in any order.
    """
    display_name = "Open Missions"
    range_start = 1
    range_end = 15
    default = 1


class FinalMissionWins(Range):
    """With goal: set_missions_and_final_mission, how many other missions of a side open its final mission."""
    display_name = "Wins Before the Final Mission"
    range_start = 0
    range_end = 15
    default = 10


class LogicStrictness(Choice):
    """
    What logic expects you to have before a mission (the "need" and "recommended" lines of the apworld's
    logic.txt, written from a community walkthrough).
    normal: what the walkthrough uses ("need" and "recommended").
    relaxed: only the bare minimum ("need").
    """
    display_name = "Logic Strictness"
    option_normal = 0
    option_relaxed = 1
    default = 0


class BoosterBuilds(DefaultOnToggle):
    """Building a robot with each of the nine boosters for the first time is a check."""
    display_name = "Booster Builds"


class ComboBuilds(Toggle):
    """
    Build-sanity: every weapon + legs combination of your side built for the first time is a check (64 checks),
    e.g. "Build Rocket Launcher + Caterpillar".
    """
    display_name = "Combo Builds"


class ShopChecks(DefaultOnToggle):
    """
    Every purchase from the engineer between missions is a check: the 9 boosters, the 8 base enhancements and the
    3 secret inventions. What you get is decided by the items you receive instead.
    """
    display_name = "Shop Checks"


class SpyChecks(Choice):
    """
    The spy's report on the enemy, which can be bought once before each mission.
    off: no check.
    check: buying it is a check.
    check_and_hint: buying it is a check and also hints one of your items from the multiworld.
    """
    display_name = "Spy Checks"
    option_off = 0
    option_check = 1
    option_check_and_hint = 2
    default = 1


class AlienArtifacts(Range):
    """With the alien artifact goals, how many Alien Artifacts are needed."""
    display_name = "Alien Artifacts Required"
    range_start = 1
    range_end = 30
    default = 10


class ExtraArtifacts(Range):
    """With the alien artifact goals, how many Alien Artifacts are in the pool beyond the required ones."""
    display_name = "Extra Alien Artifacts"
    range_start = 0
    range_end = 10
    default = 2


class TrapPercentage(Range):
    """How many of the filler items are traps, in percent. A trap hits a random robot of yours in the current
    mission (or the next one)."""
    display_name = "Trap Percentage"
    range_start = 0
    range_end = 100
    default = 0


class OilTrapWeight(Range):
    """How likely a trap is an Oil Trap: an enemy oil slick under one of your robots."""
    display_name = "Oil Trap Weight"
    range_start = 0
    range_end = 10
    default = 3


class FreezeTrapWeight(Range):
    """How likely a trap is a Freeze Trap: one of your robots is frozen like by a Nitrogrenade."""
    display_name = "Freeze Trap Weight"
    range_start = 0
    range_end = 10
    default = 3


class DisruptorTrapWeight(Range):
    """How likely a trap is a Disruptor Trap: one of your robots malfunctions like after a Disrupter hit."""
    display_name = "Disruptor Trap Weight"
    range_start = 0
    range_end = 10
    default = 3


class MineTrapWeight(Range):
    """How likely a trap is a Mine Trap: one of your robots is destroyed after 30 seconds."""
    display_name = "Mine Trap Weight"
    range_start = 0
    range_end = 10
    default = 2


class NukeTrapWeight(Range):
    """How likely a trap is a Nuke Trap: an enemy Nuke goes off at one of your robots."""
    display_name = "Nuke Trap Weight"
    range_start = 0
    range_end = 10
    default = 1


@dataclass
class RoboRumbleOptions(PerGameCommonOptions):
    side: Side
    starting_side: StartingSide
    starting_parts: StartingParts
    starting_weapons: StartingWeapons
    goal: Goal
    open_missions: OpenMissions
    final_mission_wins: FinalMissionWins
    logic_strictness: LogicStrictness
    booster_builds: BoosterBuilds
    combo_builds: ComboBuilds
    shop_checks: ShopChecks
    spy_checks: SpyChecks
    alien_artifacts: AlienArtifacts
    extra_artifacts: ExtraArtifacts
    trap_percentage: TrapPercentage
    oil_trap_weight: OilTrapWeight
    freeze_trap_weight: FreezeTrapWeight
    disruptor_trap_weight: DisruptorTrapWeight
    mine_trap_weight: MineTrapWeight
    nuke_trap_weight: NukeTrapWeight
    death_link: DeathLink
