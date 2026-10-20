"""
RoboRumble client: connects the Archipelago server and the game patch (winmm.dll, patch/src/ap.c).

The patch connects to this client on 127.0.0.1:<bridge_port> and both sides send text lines (see ap.c for the
protocol): the client sends the slot, every received item and the checked locations; the game sends checks.
"""
import asyncio
import os
import random
import sys
import urllib.parse

from typing import Any, Dict, List, Optional, Set

import CommonClient
import Utils

from . import data, game_install

tracker_loaded = False
try:
    from worlds.tracker.TrackerClient import TrackerGameContext as Context
    from worlds.tracker.TrackerClient import TrackerCommandProcessor as CommandProcessor

    tracker_loaded = True
except ModuleNotFoundError:
    from CommonClient import CommonContext as Context
    from CommonClient import ClientCommandProcessor as CommandProcessor

DEFAULT_PORT = 38480

DEV_HELP = (
    "/dev trap <oil|freeze|disruptor|mine|nuke>: set off a trap on one of your robots",
    "/dev win, /dev lose: end the mission you are playing",
    "/dev money <amount>: set your Money for the shop; /dev money +<amount> adds to it",
    "/dev god [on|off]: your robots take no damage and can't be frozen, your base keeps its energy",
    "/dev nolose [on|off]: missions can't be lost (also the hidden rules like \"lose 3 robos\")",
    "/dev funds [on|off]: endless Robo Funds in missions (for building robots)",
    "/dev infmoney [on|off]: endless Money for the shop",
    "/dev alien <speed|defense|weapon|intelligence> [all]: drop an alien booster next to one of your robots (a",
    "    selected one if any); with \"all\" every robot of yours gets it at once",
    "    (god, nolose, funds and infmoney stay on until turned off, also when the game restarts)",
    "/dev give <item>: give yourself an item in the game only (part, booster, upgrade, invention, mission access,",
    "    artifact, Robo Funds); the server doesn't know about it. /dev give all: every part and booster.",
    "    /dev give clear: take them back (closing the client does too)",
    "Building or buying with given items still sends their checks.",
)


DEV_TOGGLES = {"god": "GOD", "nolose": "NOLOSE", "funds": "FUNDS", "infmoney": "INFMONEY"}


def dev_givable() -> Dict[str, int]:
    """Items /dev give can give: everything but filler (Money is counted once per item) and traps."""
    skip = set(data.FILLER.values()) | set(data.TRAPS.values())
    return {name: item_id for name, item_id in data.item_table().items() if name not in skip}


class RoboRumbleCommandProcessor(CommandProcessor):
    ctx: "RoboRumbleContext"

    def _cmd_mission(self, number: str = "", side: str = "") -> None:
        """Choose the mission to play next: /mission <1-16> [redstar|terraflux]. Without a number: lists them."""
        ctx = self.ctx
        sides = ctx.sides()
        if not number:
            for s in sides:
                unlocked = ctx.unlocked_missions(s)
                done = {n for n in unlocked if data.mission_id(s, n) in ctx.checked_locations}
                self.output(f"{data.SIDES[s]}: " + ", ".join(f"{n}{' (won)' if n in done else ''}" for n in unlocked))
            return
        try:
            n = int(number)
        except ValueError:
            self.output("Usage: /mission <1-16> [redstar|terraflux]")
            return
        s = sides[0]
        if side:
            s = 1 if side.lower().startswith("t") else 0
        if s not in sides or n not in ctx.unlocked_missions(s):
            self.output(f"{data.SIDES[s]} mission {n} is still locked.")
            return
        Utils.async_start(ctx.game_send(f"MISSION {s} {n}"))
        self.output(f"{data.SIDES[s]} mission {n} is played next (in a briefing it loads right away).")

    def _cmd_dev(self, *args: str) -> None:
        """Dev commands for testing, /dev help lists them. Needs roborumble_options.dev_mode in host.yaml."""
        ctx = self.ctx
        if not ctx.world_settings("dev_mode", False):
            self.output("Dev commands are off. Set roborumble_options.dev_mode to true in host.yaml.")
            return
        sub = args[0].lower() if args else "help"
        rest = " ".join(args[1:]).strip()
        if sub == "help":
            for line in DEV_HELP:
                self.output(line)
            return
        if ctx.game_writer is None:
            self.output("The game is not connected.")
            return
        lines: List[str] = []
        if sub in DEV_TOGGLES:
            value = rest.lower()
            on = not ctx.dev_toggles.get(sub, False) if value == "" else value in ("on", "1", "true", "yes")
            ctx.dev_toggles[sub] = on
            lines.append(f"DEV SET {DEV_TOGGLES[sub]} {int(on)}")
            self.output(f"{sub}: {'on' if on else 'off'}")
        elif sub == "alien":
            kinds = ("speed", "defense", "weapon", "intelligence")
            words = rest.lower().split()
            if not words or words[0] not in kinds:
                self.output("Usage: /dev alien <speed|defense|weapon|intelligence> [all]")
                return
            every = len(words) > 1 and words[1] == "all"
            lines.append(f"DEV ALIEN {kinds.index(words[0]) + 1}" + (" ALL" if every else ""))
            self.output(f"Alien {words[0].capitalize()} Booster: " +
                        ("given to all your robots." if every else "dropped next to one of your robots."))
        elif sub == "trap":
            names = [name.split()[0].lower() for name in data.TRAPS.values()]
            if rest.lower() not in names:
                self.output("Usage: /dev trap <" + "|".join(names) + ">")
                return
            lines.append(f"DEV TRAP {names.index(rest.lower())}")
            self.output(f"{rest.capitalize()} trap: goes off on one of your robots (in the next mission if you aren't in one).")
        elif sub in ("win", "lose"):
            lines.append(f"DEV {sub.upper()}")
            self.output(f"The mission is {'won' if sub == 'win' else 'lost'}.")
        elif sub == "money":
            try:
                amount = int(rest.replace(",", "").replace(" ", "").lstrip("+"))
            except ValueError:
                self.output("Usage: /dev money <amount> sets it, /dev money +<amount> adds to it.")
                return
            lines.append(f"DEV {'ADDMONEY' if rest.startswith('+') else 'MONEY'} {amount}")
            self.output(f"Shop money {'+' if rest.startswith('+') else '= '}{amount:,}.")
        elif sub == "give":
            givable = dev_givable()
            if rest.lower() == "clear":
                lines.append("DEV CLEAR")
                ctx.dev_items.clear()
                self.output("The items given with /dev give are gone again.")
            elif rest.lower() == "all":
                parts = list(data.HEADS) + list(data.LEGS) + list(data.BOOSTERS)
                lines += [f"DEV GIVE {givable[name]}" for name in parts]
                ctx.dev_items += [givable[name] for name in parts]
                self.output("Given: every weapon, legs and booster.")
            else:
                name, usable, response = Utils.get_intended_text(rest, list(givable))
                containing = [n for n in givable if rest.lower() in n.lower()]
                if not usable and len(containing) == 1:
                    name, usable = containing[0], True
                if not usable:
                    self.output(response)
                    return
                lines.append(f"DEV GIVE {givable[name]}")
                ctx.dev_items.append(givable[name])
                self.output(f"Given: {name}.")
        else:
            self.output(f"Unknown dev command {sub}. /dev help lists them.")
            return
        for line in lines:
            Utils.async_start(ctx.game_send(line))

    def _cmd_alien(self, kind: str = "") -> None:
        """Drop one of your Alien Booster items next to your selected robot: /alien <speed|defense|weapon|intelligence>.
        Without a name: how many you have."""
        ctx = self.ctx
        if not kind:
            for i, name in enumerate(data.ALIEN_BOOSTERS.values()):
                self.output(f"{name}: {ctx.aliens_left(i)} left")
            return
        kinds = [name.split()[1].lower() for name in data.ALIEN_BOOSTERS.values()]
        if kind.lower() not in kinds:
            self.output("Usage: /alien <" + "|".join(kinds) + ">")
            return
        self.output(ctx.drop_alien(kinds.index(kind.lower())))

    def _cmd_patch(self) -> None:
        """Install the game patch (winmm.dll) into the RoboRumble folder and switch Archipelago on in it."""
        self.ctx.install_patch()

    def _cmd_unpatch(self) -> None:
        """Remove the game patch from the RoboRumble folder (GOG's own winmm.dll comes back)."""
        game_path = self.ctx.game_path()
        if game_path is None:
            self.output("RoboRumble install not found. Type /path to choose the game folder.")
            return
        try:
            game_install.remove_patch(game_path)
            self.output("Patch removed.")
        except OSError as e:
            self.output(f"Could not remove the patch (is the game running?): {e}")

    def _cmd_path(self, *folder: str) -> None:
        """Set the RoboRumble folder (GOG or Steam). Without a folder: opens a folder picker."""
        if folder:
            self.ctx.set_game_path(" ".join(folder).strip('"'))
        else:
            Utils.async_start(self.ctx.pick_game_path())

    def _cmd_run(self, renderer: str = "") -> None:
        """Start RoboRumble, patching it first if needed. Optional renderer: glide, software or direct3d."""
        self.ctx.launch_game(renderer)

    def _cmd_game(self) -> None:
        """Show the state of the game connection."""
        ctx = self.ctx
        game_path = ctx.game_path()
        self.output(f"Install: {game_path or 'not found'}")
        if game_path:
            self.output(f"Release: {'GOG' if game_install.is_gog(game_path) else 'Steam / other'}, "
                        f"patch up to date: {game_install.is_patch_current(game_path)}, "
                        f"default renderer: {game_install.default_renderer(game_path)}")
        self.output(f"Game connected: {ctx.game_writer is not None} (port {ctx.port})")
        if ctx.game_state:
            self.output(f"Game mode {ctx.game_state[0]}, mission {ctx.game_state[1]}")


class RoboRumbleContext(Context):
    tags: Set[str] = {"AP"}
    game = data.GAME_NAME
    command_processor = RoboRumbleCommandProcessor
    items_handling = 0b111
    want_slot_data = True

    def __init__(self, server_address: Optional[str], password: Optional[str]) -> None:
        super().__init__(server_address, password)
        self.slot_data: Dict[str, Any] = {}
        self.port = int(self.world_settings("bridge_port", DEFAULT_PORT))
        self.game_writer: Optional[asyncio.StreamWriter] = None
        self.items_announced = 0   # received items already shown in the game
        self.game_server: Optional[asyncio.AbstractServer] = None
        self.game_state: Optional[List[int]] = None
        self.shop_texts: Dict[int, Any] = {}   # shop slot -> (flags, "Item for Player")
        self.shop_hinted: Set[int] = set()
        self.money_state: Optional[Dict[str, int]] = None   # data storage: the campaign's Money, Money items added
        self.money_loaded = False
        self.slot_synced = False   # SLOT went to the game on this connection (MONEYSTATE comes after it)
        self.aliens_used: List[int] = [0, 0, 0, 0]           # data storage: Alien Booster items dropped
        self.aliens_pending: List[int] = [0, 0, 0, 0]        # sent to the game, not dropped yet
        self.dev_items: List[int] = []
        # Universal Tracker: which locations are in logic (shown per mission in the RoboRumble tab)
        self.locations_in_logic: List[str] = []
        self.locations_out_of_logic: List[str] = []
        if tracker_loaded:
            def in_logic(names: List[str]) -> None:
                self.locations_in_logic = names

            def out_of_logic(names: List[str]) -> None:
                self.locations_out_of_logic = names

            self.update_callback = in_logic
            self.glitches_callback = out_of_logic         # items given with /dev give (the game counts them too)
        self.dev_toggles: Dict[str, bool] = {}   # /dev god etc., sent again when the game reconnects

    def make_gui(self):
        ui = super().make_gui()

        class RoboRumbleManager(ui):
            base_title = "Archipelago RoboRumble Client"

            def build(self):
                container = super().build()
                from .gui import RoboRumbleTab
                self.add_client_tab("RoboRumble", RoboRumbleTab(self.ctx))
                return container

        return RoboRumbleManager

    @staticmethod
    def world_settings(key: str, default: Any) -> Any:
        try:
            from .world import RoboRumbleWorld
            return getattr(RoboRumbleWorld.settings, key)
        except Exception:
            return default

    def game_path(self) -> Optional[str]:
        return game_install.find_game_path(str(self.world_settings("game_path", "") or ""))

    def set_game_path(self, folder: str) -> bool:
        """Remembers the game folder in host.yaml (roborumble_options.game_path)."""
        folder = os.path.normpath(folder)
        if os.path.isfile(folder):                      # an exe was picked
            folder = os.path.dirname(folder)
        if os.path.basename(folder).lower() == "data":  # the DATA folder was picked
            folder = os.path.dirname(folder)
        if not game_install.is_game_path(folder):
            CommonClient.logger.error(f"{folder} is not a RoboRumble folder (it should have DATA/rr_soft.exe in it).")
            return False
        try:
            from settings import get_settings
            from .world import RoboRumbleSettings, RoboRumbleWorld
            RoboRumbleWorld.settings.game_path = RoboRumbleSettings.GamePath(folder)
            get_settings().save()
        except Exception as e:
            CommonClient.logger.error(f"Could not save the folder in host.yaml: {e}")
            return False
        CommonClient.logger.info(f"RoboRumble folder set to {folder} "
                                 f"({'GOG' if game_install.is_gog(folder) else 'Steam / other'}). /run starts it.")
        return True

    async def pick_game_path(self) -> None:
        start = self.game_path() or ""
        folder = await asyncio.to_thread(Utils.open_directory, "RoboRumble folder (GOG or Steam)", start)
        if folder:
            self.set_game_path(folder)

    def install_patch(self) -> bool:
        game_path = self.game_path()
        if game_path is None:
            CommonClient.logger.error("RoboRumble install not found. Type /path to choose the game folder.")
            return False
        try:
            game_install.install_patch(game_path, self.port)
        except (OSError, RuntimeError) as e:
            CommonClient.logger.error(f"Could not install the patch (is the game running?): {e}")
            return False
        CommonClient.logger.info(f"Patch installed into {os.path.join(game_path, 'DATA')}.")
        return True

    def launch_game(self, renderer: str = "") -> None:
        game_path = self.game_path()
        if game_path is None:
            CommonClient.logger.error("RoboRumble install not found. Type /path to choose the game folder.")
            return
        if not game_install.is_patch_current(game_path) and not self.install_patch():
            return
        renderer = (renderer or str(self.world_settings("renderer", "auto") or "auto")).lower()
        if renderer == "auto":
            renderer = game_install.default_renderer(game_path)
        if renderer not in game_install.RENDERERS:
            CommonClient.logger.error("Renderer must be glide, software or direct3d.")
            return
        if renderer == "glide" and not game_install.has_glide_wrapper(game_path):
            CommonClient.logger.warning("No Glide wrapper found (install nGlide); starting the software renderer.")
            renderer = "software"
        try:
            game_install.launch(game_path, renderer)
        except OSError as e:
            CommonClient.logger.error(f"Could not start the game: {e}")
            return
        CommonClient.logger.info(f"Starting RoboRumble ({renderer}).")

    # ------------------------------------------------------------ server side
    async def server_auth(self, password_requested: bool = False):
        if password_requested and not self.password:
            await super().server_auth(password_requested)
        await self.get_username()
        await self.send_connect()

    # ------------------------------------------------------------ data storage
    def storage_key(self, what: str) -> str:
        return f"RoboRumble_{self.team}_{self.slot}_{what}"

    async def load_storage(self) -> None:
        self.money_loaded = False
        await self.send_msgs([{"cmd": "Get", "keys": [self.storage_key("money"), self.storage_key("aliens_used")]}])

    async def save_storage(self, what: str, value: Any) -> None:
        await self.send_msgs([{"cmd": "Set", "key": self.storage_key(what), "default": None, "want_reply": False,
                               "operations": [{"operation": "replace", "value": value}]}])

    async def send_money_state(self) -> None:
        if self.money_loaded and self.slot_synced and self.game_writer is not None:
            money = self.money_state or {}
            await self.game_send(f"MONEYSTATE {int(money.get('money', -1))} {int(money.get('items', -1))}")

    # ------------------------------------------------------------ alien boosters
    def aliens_received(self, i: int) -> int:
        item_id = data.ALIEN_OFFSET + i
        return sum(1 for item in self.items_received if item.item == item_id)

    def aliens_left(self, i: int) -> int:
        return self.aliens_received(i) - self.aliens_used[i] - self.aliens_pending[i]

    def drop_alien(self, i: int) -> str:
        """Asks the game to drop Alien Booster i (0 Speed .. 3 Intelligence); returns what to tell the player."""
        name = list(data.ALIEN_BOOSTERS.values())[i]
        if self.aliens_left(i) <= 0:
            return f"No {name} left."
        if self.game_writer is None:
            return "The game is not connected."
        self.aliens_pending[i] += 1
        Utils.async_start(self.game_send(f"ALIEN {i + 1}"))
        in_mission = bool(self.game_state) and self.game_state[0] in (4, 5)
        return (f"{name}: dropped next to your selected robot." if in_mission
                else f"{name}: dropped next to one of your robots in the next mission.")

    def on_package(self, cmd: str, args: Any) -> None:
        if cmd == "Connected":
            self.slot_data = args.get("slot_data", {})
            Utils.async_start(self.load_storage())
            Utils.async_start(self.send_msgs([{"cmd": "StatusUpdate",
                                                "status": CommonClient.ClientStatus.CLIENT_PLAYING}]))
            Utils.async_start(self.sync_game())
        elif cmd == "ReceivedItems":
            Utils.async_start(self.announce_items())
            Utils.async_start(self.send_items())
            Utils.async_start(self.check_goal())
        elif cmd == "RoomUpdate" and "checked_locations" in args:
            Utils.async_start(self.send_checked())
        elif cmd == "LocationInfo":
            Utils.async_start(self.shop_labels(args.get("locations", [])))
        elif cmd == "Retrieved":
            keys = args.get("keys", {})
            if self.storage_key("money") in keys:
                self.money_state = keys[self.storage_key("money")]
                self.money_loaded = True
                Utils.async_start(self.send_money_state())
            used = keys.get(self.storage_key("aliens_used"))
            if isinstance(used, list) and len(used) == 4:
                self.aliens_used = [int(v) for v in used]
        super().on_package(cmd, args)

    def sides(self) -> List[int]:
        side = int(self.slot_data.get("side", 0))
        return [0, 1] if side == 2 else [side]

    def goal(self) -> int:
        """0 final_mission_only, 1 set_missions_and_final_mission, 2 alien_artifacts_only, 3 alien artifacts + final"""
        return int(self.slot_data.get("goal", 1))

    def artifacts(self) -> int:
        return sum(1 for item in self.items_received if item.item == data.ARTIFACT_ID)

    def unlocked_missions(self, side: int) -> List[int]:
        received = {item.item for item in self.items_received} | set(self.dev_items)
        wins = sum(1 for n in range(1, data.FINAL_MISSION) if data.mission_id(side, n) in self.checked_locations)
        start = int(self.slot_data.get("start_side", self.sides()[0]))
        unlocked = [n for n in range(1, data.FINAL_MISSION)
                    if (n == 1 and side == start) or data.mission_id(side, n) in received]
        goal = self.goal()
        if ((goal == 1 and wins >= int(self.slot_data.get("final_mission_wins", 10)))
                or (goal == 3 and self.artifacts() >= int(self.slot_data.get("alien_artifacts", 10)))
                or (goal in (0, 2) and data.mission_id(side, data.FINAL_MISSION) in received)):
            unlocked.append(data.FINAL_MISSION)
        return unlocked

    async def check_goal(self) -> None:
        if self.finished_game or not self.slot:
            return
        if self.goal() == 2:
            # the robot offers the epilogue; the goal counts once it is watched (EPILOGUE from the game)
            if self.artifacts() >= int(self.slot_data.get("alien_artifacts", 10)):
                await self.game_send("GOAL")
            return
        if any(data.mission_id(s, data.FINAL_MISSION) not in self.checked_locations for s in self.sides()):
            return
        self.finished_game = True
        await self.send_msgs([{"cmd": "StatusUpdate", "status": CommonClient.ClientStatus.CLIENT_GOAL}])
        CommonClient.logger.info("Goal reached!")

    # ------------------------------------------------------------ game side
    async def game_send(self, line: str) -> None:
        if self.game_writer is None:
            return
        try:
            self.game_writer.write((line + "\n").encode("ascii", "replace"))
            await self.game_writer.drain()
        except (ConnectionError, OSError):
            self.game_writer = None

    async def sync_game(self) -> None:
        if not self.slot or not self.slot_data or self.game_writer is None:
            return
        side = int(self.slot_data.get("side", 0))
        final_wins = int(self.slot_data.get("final_mission_wins", 10))
        start_side = int(self.slot_data.get("start_side", 0 if side == 2 else side))
        seed_name = getattr(self, "server_seed_name", None) or self.seed_name or "seed"
        seed = "".join(c for c in seed_name if c.isalnum())[:40] + f"_{self.slot}"
        goal = self.goal()
        artifacts = int(self.slot_data.get("alien_artifacts", 10))
        await self.game_send(f"SLOT {side} {final_wins} {start_side} {seed} {goal} {artifacts}")
        rows = list(self.slot_data.get("rows_heads", range(16))) + list(self.slot_data.get("rows_legs", range(16)))
        await self.game_send("ROWS " + " ".join(str(int(v)) for v in rows))
        self.slot_synced = True
        await self.send_money_state()
        self.aliens_pending = [0, 0, 0, 0]   # a new game process: drops it didn't make are gone
        if self.world_settings("dev_mode", False):
            for name, on in self.dev_toggles.items():
                await self.game_send(f"DEV SET {DEV_TOGGLES[name]} {int(on)}")
            await self.game_send("DEV CLEAR")
            for item_id in self.dev_items:
                await self.game_send(f"DEV GIVE {item_id}")
        if goal == 2 and self.artifacts() >= artifacts:
            await self.game_send("GOAL")
        await self.send_checked()
        await self.send_items()
        await self.scout_shop()

    # ------------------------------------------------------------ the engineer's shop
    def shop_locations(self) -> Dict[int, Any]:
        """location id -> (side, the game's shop slot) for this slot's shop checks"""
        return {loc: slot for loc, slot in data.shop_slots().items()
                if loc in self.missing_locations or loc in self.checked_locations}

    async def scout_shop(self) -> None:
        """What the shop's goods are, for its labels (no hint yet)"""
        locations = sorted(self.shop_locations())
        if locations:
            await self.send_msgs([{"cmd": "LocationScouts", "locations": locations, "create_as_hint": 0}])

    async def shop_labels(self, items: List[Any]) -> None:
        slots = self.shop_locations()
        for item in items:
            slot = slots.get(item.location)
            if slot is None:
                continue
            name = self.item_names.lookup_in_slot(item.item, item.player)
            text = f"{name} for {self.player_names.get(item.player, '?')}"[:60]
            self.shop_texts[slot] = (item.flags, text)
            await self.game_send(f"SHOPITEM {slot[0]} {slot[1]} {item.flags} {text}")

    async def shop_opened(self, side: int) -> None:
        """The engineer's menu opened in a campaign of that side: what is left in its shop gets hinted (once a
        session per side)"""
        locations = sorted(loc for loc, (s, _) in self.shop_locations().items()
                           if s == side and loc in self.missing_locations)
        if locations and side not in self.shop_hinted:
            self.shop_hinted.add(side)
            await self.send_msgs([{"cmd": "LocationScouts", "locations": locations, "create_as_hint": 2}])

    def on_print_json(self, args: dict) -> None:
        super().on_print_json(args)
        # this slot's items that went to someone else are announced in the game too
        item = args.get("item")
        receiving = args.get("receiving")
        if args.get("type") == "ItemSend" and item is not None and item.player == self.slot and receiving != self.slot:
            name = self.item_names.lookup_in_slot(item.item, receiving)
            Utils.async_start(self.game_send(f"MSG Sent {name} to {self.player_names.get(receiving, '?')}"))

    async def announce_items(self) -> None:
        """New items in the game's message line. A fresh connection only marks what is already there."""
        if self.game_writer is None:
            return
        new = self.items_received[self.items_announced:]
        self.items_announced = len(self.items_received)
        for item in new[-8:]:
            name = self.item_names.lookup_in_game(item.item)
            if item.player == self.slot:
                await self.game_send(f"MSG Found {name}")
            else:
                await self.game_send(f"MSG Received {name} from {self.player_names.get(item.player, '?')}")

    async def send_items(self) -> None:
        if self.slot:
            await self.game_send("ITEMS " + ",".join(str(item.item) for item in self.items_received))

    async def send_checked(self) -> None:
        if self.slot:
            await self.game_send("CHECKED " + ",".join(str(loc) for loc in sorted(self.checked_locations)))

    async def on_game_line(self, line: str) -> None:
        cmd, _, rest = line.partition(" ")
        if cmd == "HELLO":
            CommonClient.logger.info(f"Game connected ({rest}).")
            self.slot_synced = False
            self.items_announced = len(self.items_received)
            await self.sync_game()
        elif cmd == "CHECK" and self.slot:
            location = int(rest)
            if location in self.missing_locations or location in self.checked_locations:
                await self.check_locations([location])
                if data.SPY_OFFSET < location <= data.SPY_OFFSET + 100 + data.MISSIONS and \
                        int(self.slot_data.get("spy_checks", 0)) == 2:
                    await self.spy_hint()
                await self.check_goal()
        elif cmd == "EPILOGUE" and self.slot and not self.finished_game:
            self.finished_game = True
            await self.send_msgs([{"cmd": "StatusUpdate", "status": CommonClient.ClientStatus.CLIENT_GOAL}])
            CommonClient.logger.info("Goal reached: the epilogue was watched!")
        elif cmd == "STATE":
            self.game_state = [int(v) for v in rest.split()]
        elif cmd == "MONEY" and self.slot:
            money, items = (int(v) for v in rest.split())
            self.money_state = {"money": money, "items": items}
            await self.save_storage("money", self.money_state)
        elif cmd == "ALIENUSED" and self.slot:
            i = int(rest) - 1
            if 0 <= i < 4:
                self.aliens_pending[i] = max(0, self.aliens_pending[i] - 1)
                self.aliens_used[i] += 1
                await self.save_storage("aliens_used", self.aliens_used)
        elif cmd == "SHOP" and self.slot:
            await self.shop_opened(int(rest or 0))

    async def spy_hint(self) -> None:
        """The spy also tells where one of this slot's own checks leads."""
        candidates = sorted(self.missing_locations - set(self.locations_scouted))
        if candidates:
            location = random.choice(candidates)
            await self.send_msgs([{"cmd": "LocationScouts", "locations": [location], "create_as_hint": 2}])

    async def handle_game(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        if self.game_writer is not None:
            self.game_writer.close()
        self.game_writer = writer
        try:
            while True:
                raw = await reader.readline()
                if not raw:
                    break
                await self.on_game_line(raw.decode("ascii", "replace").strip())
        except (ConnectionError, OSError):
            pass
        finally:
            if self.game_writer is writer:
                self.game_writer = None
            CommonClient.logger.info("Game disconnected.")

    async def start_game_server(self) -> None:
        try:
            self.game_server = await asyncio.start_server(self.handle_game, "127.0.0.1", self.port)
            CommonClient.logger.info(f"Waiting for RoboRumble on port {self.port}.")
        except OSError as e:
            CommonClient.logger.error(f"Could not listen on port {self.port} for the game: {e}")


def main(*args) -> None:
    Utils.init_logging("RoboRumbleClient", exception_logger="Client")
    parser = CommonClient.get_base_parser(description="RoboRumble Client")
    parser.add_argument("url", nargs="?", help="Archipelago Connection URL")
    parser.add_argument("--name", default=None, help="Archipelago Slot Name")
    args = parser.parse_args(args)
    if args.url:
        url = urllib.parse.urlparse(args.url)
        args.connect = url.netloc
        if url.username:
            args.name = urllib.parse.unquote(url.username)
        if url.password:
            args.password = urllib.parse.unquote(url.password)

    async def _main(_args):
        ctx = RoboRumbleContext(_args.connect, _args.password)
        ctx.auth = _args.name
        ctx.server_task = asyncio.create_task(CommonClient.server_loop(ctx), name="server loop")
        await ctx.start_game_server()
        if tracker_loaded:
            ctx.run_generator()
        if CommonClient.gui_enabled:
            ctx.run_gui()
        ctx.run_cli()
        await ctx.exit_event.wait()
        if ctx.game_server is not None:
            ctx.game_server.close()
        await ctx.shutdown()

    import colorama
    colorama.just_fix_windows_console()
    asyncio.run(_main(args))
    colorama.deinit()


if __name__ == "__main__":
    main(*sys.argv[1:])
