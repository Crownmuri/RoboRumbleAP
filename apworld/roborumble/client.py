"""
RoboRumble client: connects the Archipelago server and the game patch (winmm.dll, patch/src/ap.c).

The patch connects to this client on 127.0.0.1:<bridge_port> and both sides send text lines (see ap.c for the
protocol): the client sends the slot, every received item and the checked locations; the game sends checks.
"""
import asyncio
import random
import sys
import urllib.parse

from typing import Any, Dict, List, Optional, Set

import CommonClient
import Utils

from . import data

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

    def _cmd_game(self) -> None:
        """Show the state of the game connection."""
        ctx = self.ctx
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
        self.port = DEFAULT_PORT
        try:
            from .world import RoboRumbleWorld
            self.port = int(getattr(RoboRumbleWorld.settings, "bridge_port", DEFAULT_PORT))
        except Exception:
            pass
        self.game_writer: Optional[asyncio.StreamWriter] = None
        self.game_server: Optional[asyncio.AbstractServer] = None
        self.game_state: Optional[List[int]] = None

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
            Utils.async_start(self.send_items())
            Utils.async_start(self.check_goal())
        elif cmd == "RoomUpdate" and "checked_locations" in args:
            Utils.async_start(self.send_checked())
        super().on_package(cmd, args)

    def sides(self) -> List[int]:
        side = int(self.slot_data.get("side", 0))
        return [0, 1] if side == 2 else [side]

    def unlocked_missions(self, side: int) -> List[int]:
        received = {item.item for item in self.items_received}
        wins = sum(1 for n in range(1, data.FINAL_MISSION) if data.mission_id(side, n) in self.checked_locations)
        unlocked = [n for n in range(1, data.FINAL_MISSION) if n == 1 or data.mission_id(side, n) in received]
        if wins >= int(self.slot_data.get("final_mission_wins", 10)):
            unlocked.append(data.FINAL_MISSION)
        return unlocked

    async def check_goal(self) -> None:
        if self.finished_game or not self.slot:
            return
        if any(data.mission_id(s, data.FINAL_MISSION) not in self.checked_locations for s in self.sides()):
            return
        artifacts = sum(1 for item in self.items_received if item.item == data.ARTIFACT_ID)
        if artifacts >= int(self.slot_data.get("alien_artifacts", 0)):
            self.finished_game = True
            await self.send_msgs([{"cmd": "StatusUpdate", "status": CommonClient.ClientStatus.CLIENT_GOAL}])
            CommonClient.logger.info("Goal reached: the Aliens are defeated!")

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
        seed_name = getattr(self, "server_seed_name", None) or self.seed_name or "seed"
        seed = "".join(c for c in seed_name if c.isalnum())[:40] + f"_{self.slot}"
        await self.game_send(f"SLOT {side} {final_wins} {seed}")
        await self.send_checked()
        await self.send_items()

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
            await self.sync_game()
        elif cmd == "CHECK" and self.slot:
            location = int(rest)
            if location in self.missing_locations or location in self.checked_locations:
                await self.check_locations([location])
                if data.SPY_OFFSET < location <= data.SPY_OFFSET + 100 + data.MISSIONS and \
                        int(self.slot_data.get("spy_checks", 0)) == 2:
                    await self.spy_hint()
                await self.check_goal()
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
