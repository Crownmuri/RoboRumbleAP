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


@dataclass
class RoboRumbleOptions(PerGameCommonOptions):
    side: Side
    starting_parts: StartingParts
    open_missions: OpenMissions
    logic_strictness: LogicStrictness
    booster_builds: BoosterBuilds
    combo_builds: ComboBuilds
    death_link: DeathLink
