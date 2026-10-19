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
        self.output(f"{data.SIDES[s]} mission {n} is played next, from the next briefing on.")

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

    def on_package(self, cmd: str, args: Any) -> None:
        if cmd == "Connected":
            self.slot_data = args.get("slot_data", {})
            Utils.async_start(self.send_msgs([{"cmd": "StatusUpdate",
                                                "status": CommonClient.ClientStatus.CLIENT_PLAYING}]))
            Utils.async_start(self.sync_game())
        elif cmd == "ReceivedItems":
            Utils.async_start(self.announce_items())
            Utils.async_start(self.send_items())
            Utils.async_start(self.check_goal())
        elif cmd == "RoomUpdate" and "checked_locations" in args:
            Utils.async_start(self.send_checked())
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
        received = {item.item for item in self.items_received}
        wins = sum(1 for n in range(1, data.FINAL_MISSION) if data.mission_id(side, n) in self.checked_locations)
        start = int(self.slot_data.get("start_side", self.sides()[0]))
        unlocked = [n for n in range(1, data.FINAL_MISSION)
                    if (n == 1 and side == start) or data.mission_id(side, n) in received]
        goal = self.goal()
        if (goal == 1 and wins >= int(self.slot_data.get("final_mission_wins", 10))) or                 (goal == 3 and self.artifacts() >= int(self.slot_data.get("alien_artifacts", 10))) or                 (goal in (0, 2) and data.mission_id(side, data.FINAL_MISSION) in received):
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
        if goal == 2 and self.artifacts() >= artifacts:
            await self.game_send("GOAL")
        await self.send_checked()
        await self.send_items()

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
