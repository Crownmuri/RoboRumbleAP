from dataclasses import dataclass

from Options import Choice, DeathLink, DefaultOnToggle, PerGameCommonOptions, Range, Toggle


class Side(Choice):
    """
    The campaign to play. Each side has its own 16 missions and its own eight weapons and eight legs.
    Red Star: Chain Gun, Rocket Launcher, ... Nuke. Terraflux: Laser, Pulse Gun, ... Infector.
    """
    display_name = "Side"
    option_red_star = 0
    option_terraflux = 1
    default = 0


class StartingParts(Choice):
    """
    What you can build at the start.
    vanilla: the weapons and legs the first mission gives in the original game.
    minimal: one random weapon and the side's basic legs.
    """
    display_name = "Starting Parts"
    option_vanilla = 0
    option_minimal = 1
    default = 0


class OpenMissions(Range):
    """
    How many missions are open from the start (mission 1 always is). Every other mission needs its Mission Access
    item; unlocked missions can be played in any order.
    """
    display_name = "Open Missions"
    range_start = 1
    range_end = 16
    default = 1


class LogicStrictness(Choice):
    """
    How many weapons and legs logic expects before a mission.
    normal: one less than the original game offers in that mission.
    relaxed: half of what the original game offers.
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
    """How many Alien Artifacts are needed before the final mission counts as the goal. 0 = none in the pool."""
    display_name = "Alien Artifacts Required"
    range_start = 0
    range_end = 20
    default = 0


class ExtraArtifacts(Range):
    """How many Alien Artifacts are in the pool beyond the required ones."""
    display_name = "Extra Alien Artifacts"
    range_start = 0
    range_end = 10
    default = 2


@dataclass
class RoboRumbleOptions(PerGameCommonOptions):
    side: Side
    starting_parts: StartingParts
    open_missions: OpenMissions
    logic_strictness: LogicStrictness
    booster_builds: BoosterBuilds
    combo_builds: ComboBuilds
    shop_checks: ShopChecks
    spy_checks: SpyChecks
    alien_artifacts: AlienArtifacts
    extra_artifacts: ExtraArtifacts
    death_link: DeathLink
