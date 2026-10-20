"""
The client's RoboRumble tab: a mission map per side (unlocked, won, checks left; with the Universal Tracker also how
many are in logic) and the Alien Booster items with a button to drop one.

Mission pictures: assets/redstar_01.png ... assets/terraflux_16.png in the apworld (any size, shown at 128x96). A
missing picture is drawn as a plain tile.
"""
import io
import pkgutil
import re
from typing import Dict, List, Optional, Tuple

import CommonClient
from kivy.clock import Clock
from kivy.core.image import Image as CoreImage
from kivy.metrics import dp
from kivy.graphics import Color, Line, Rectangle
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.button import Button
from kivy.uix.gridlayout import GridLayout
from kivy.uix.image import Image
from kivy.uix.label import Label
from kivy.uix.scrollview import ScrollView
from kivy.uix.widget import Widget

from . import data

TILE_W, TILE_H = 128, 96
SIDE_FILES = ("redstar", "terraflux")
SIDE_COLORS = ((0.55, 0.15, 0.12, 1), (0.45, 0.45, 0.10, 1))   # placeholder tiles: Red Star red, Terraflux yellow
_MISSION_LOCATION = re.compile(r"^\[(Red Star|Terraflux)\] Mission (\d+) - ")


def _picture(side: int, number: int):
    try:
        raw = pkgutil.get_data(__name__.rpartition(".")[0], f"assets/{SIDE_FILES[side]}_{number:02d}.png")
    except (OSError, ValueError):
        raw = None
    return CoreImage(io.BytesIO(raw), ext="png").texture if raw else None


def _label(text: str, size: str = "13dp", height: str = "18dp", **kw) -> Label:
    label = Label(text=text, markup=True, font_size=size, size_hint_y=None, height=height, halign="left",
                  valign="middle", **kw)
    label.bind(size=lambda lab, sz: setattr(lab, "text_size", sz))
    return label


class MissionTile(BoxLayout):
    def __init__(self, side: int, number: int) -> None:
        super().__init__(orientation="vertical", size_hint=(None, None), width=f"{TILE_W}dp", spacing="2dp")
        self.side, self.number = side, number
        texture = _picture(side, number)
        if texture is not None:
            self.picture: Widget = Image(texture=texture, size_hint=(None, None), size=(f"{TILE_W}dp", f"{TILE_H}dp"),
                                         allow_stretch=True, keep_ratio=False)
        else:
            self.picture = Label(text=f"[b]{number}[/b]", markup=True, font_size="30dp", size_hint=(None, None),
                                 size=(f"{TILE_W}dp", f"{TILE_H}dp"))
            with self.picture.canvas.before:
                Color(*SIDE_COLORS[side])
                self._bg = Rectangle(pos=self.picture.pos, size=self.picture.size)
            self.picture.bind(pos=lambda w, p: setattr(self._bg, "pos", p), size=lambda w, s: setattr(self._bg, "size", s))
        with self.picture.canvas.after:
            self._border_color = Color(0.3, 1.0, 0.5, 0)
            self._border = Line(rectangle=(0, 0, 1, 1), width=2)
        self.picture.bind(pos=self._move_border, size=self._move_border)
        self.add_widget(self.picture)
        self.title = _label(f"Mission {number}")
        self.checks = _label("")
        self.logic = _label("")
        for w in (self.title, self.checks, self.logic):
            self.add_widget(w)
        self.height = f"{TILE_H + 3 * 18 + 6}dp"

    def _move_border(self, *_) -> None:
        self._border.rectangle = (*self.picture.pos, *self.picture.size)

    def show(self, unlocked: bool, won: bool, done: int, total: int, in_logic: int, out_logic: int,
             tracker: bool, final: bool) -> None:
        self.picture.opacity = 1.0 if unlocked else 0.3
        self._border_color.a = 1.0 if won else 0.0
        name = "Final mission" if final else f"Mission {self.number}"
        self.title.text = f"[b]{name}[/b]" + ("  [color=4CFF80]won[/color]" if won else "")
        color = "888888" if done >= total else "FFFFFF"
        self.checks.text = f"[color={color}]Checks: {done}/{total}[/color]"
        if tracker and unlocked:
            self.logic.text = (f"[color=00FA9A]{in_logic}[/color] in logic" +
                               (f", [color=FFD300]{out_logic}[/color] out" if out_logic else ""))
        else:
            self.logic.text = ""


class AlienRow(BoxLayout):
    def __init__(self, ctx, i: int) -> None:
        super().__init__(orientation="horizontal", size_hint=(None, None), height="34dp", width="420dp", spacing="8dp")
        self.ctx, self.i = ctx, i
        self.name = list(data.ALIEN_BOOSTERS.values())[i]
        self.text = _label("", size="15dp", height="34dp", size_hint_x=None, width="300dp")
        self.button = Button(text="Drop", size_hint=(None, None), size=("100dp", "32dp"))
        self.button.bind(on_release=self.drop)
        self.add_widget(self.text)
        self.add_widget(self.button)

    def drop(self, *_) -> None:
        CommonClient.logger.info(self.ctx.drop_alien(self.i))

    def update(self) -> None:
        left, got = self.ctx.aliens_left(self.i), self.ctx.aliens_received(self.i)
        self.text.text = f"{self.name}: [b]{left}[/b] [color=888888](of {got})[/color]"
        self.button.disabled = left <= 0 or self.ctx.game_writer is None


class RoboRumbleTab(ScrollView):
    def __init__(self, ctx) -> None:
        super().__init__(do_scroll_x=False)
        self.ctx = ctx
        self.box = BoxLayout(orientation="vertical", size_hint_y=None, padding="8dp", spacing="6dp")
        self.box.bind(minimum_height=self.box.setter("height"))
        self.add_widget(self.box)
        self.built_for: Optional[Tuple[int, ...]] = None
        self.tiles: Dict[Tuple[int, int], MissionTile] = {}
        self.side_labels: Dict[int, Label] = {}
        self.aliens: List[AlienRow] = []
        self.grids: List[GridLayout] = []
        self.bind(width=self.fit_grids)
        self.status = _label("", size="15dp", height="24dp")
        self.status.bind(texture_size=lambda lab, ts: setattr(lab, "height", max(ts[1], dp(24))))
        Clock.schedule_interval(self.update, 0.5)

    def build(self) -> None:
        self.box.clear_widgets()
        self.tiles.clear()
        self.side_labels.clear()
        self.grids = []
        self.box.add_widget(self.status)
        hint = _label("[b]Alien Boosters[/b]  [color=888888](dropped next to your selected robot in a mission; the "
                      "robot that drives over it gets it)[/color]", size="16dp", height="28dp")
        hint.bind(texture_size=lambda lab, ts: setattr(lab, "height", max(ts[1], dp(28))))
        self.box.add_widget(hint)
        self.aliens = [AlienRow(self.ctx, i) for i in range(4)]
        for row in self.aliens:
            self.box.add_widget(row)
        for side in self.ctx.sides():
            label = _label(f"[b]{data.SIDES[side]}[/b]", size="20dp", height="34dp")
            self.side_labels[side] = label
            self.box.add_widget(label)
            grid = GridLayout(cols=8, spacing="8dp", size_hint_y=None)
            self.grids.append(grid)
            grid.bind(minimum_height=grid.setter("height"))
            for n in range(1, data.MISSIONS + 1):
                tile = MissionTile(side, n)
                self.tiles[(side, n)] = tile
                grid.add_widget(tile)
            self.box.add_widget(grid)
        self.built_for = tuple(self.ctx.sides())
        self.fit_grids()

    def fit_grids(self, *_) -> None:
        """as many mission tiles per row as fit (8 at most: two rows per side)"""
        cols = max(1, min(8, int((self.width - dp(16)) // dp(TILE_W + 8))))
        for grid in self.grids:
            grid.cols = cols

    def mission_locations(self, side: int, n: int) -> List[int]:
        """this mission's checks in this slot: clear, spy report, report bonuses"""
        ids = [data.mission_id(side, n), data.SPY_OFFSET + 100 * side + n]
        ids += [data.bonus_id(side, n, k) for k in range(len(data.BONUS_KINDS))]
        return [i for i in ids if i in self.ctx.missing_locations or i in self.ctx.checked_locations]

    def update(self, *_) -> None:
        ctx = self.ctx
        if not ctx.slot or not ctx.slot_data:
            if self.built_for != ():
                self.box.clear_widgets()
                self.box.add_widget(_label("Connect to the Archipelago server to see the missions.", size="18dp",
                                           height="40dp"))
                self.built_for = ()
            return
        if self.built_for != tuple(ctx.sides()):
            self.build()
        money = (ctx.money_state or {}).get("money")
        funds = sum(1 for item in ctx.items_received if item.item == data.ROBO_FUNDS_ID)
        parts = [f"Game: {'connected' if ctx.game_writer is not None else '[color=FF6060]not connected[/color]'}"]
        if money is not None and money >= 0:
            parts.append(f"Money: ${money:,}".replace(",", " "))
        parts.append(f"Starting Robo Funds: +{funds * data.ROBO_FUNDS_STEP}")
        if ctx.goal() in (2, 3):
            parts.append(f"Alien Artifacts: {ctx.artifacts()}/{int(ctx.slot_data.get('alien_artifacts', 10))}")
        self.status.text = "    ".join(parts)
        for row in self.aliens:
            row.update()

        from .client import tracker_loaded as tracker
        in_logic: Dict[Tuple[int, int], int] = {}
        out_logic: Dict[Tuple[int, int], int] = {}
        for names, counts in ((getattr(ctx, "locations_in_logic", []), in_logic),
                              (getattr(ctx, "locations_out_of_logic", []), out_logic)):
            for name in names or []:
                m = _MISSION_LOCATION.match(name)
                if m:
                    key = (0 if m.group(1) == "Red Star" else 1, int(m.group(2)))
                    counts[key] = counts.get(key, 0) + 1
        for side in ctx.sides():
            unlocked = set(ctx.unlocked_missions(side))
            won = 0
            for n in range(1, data.MISSIONS + 1):
                ids = self.mission_locations(side, n)
                done = sum(1 for i in ids if i in ctx.checked_locations)
                is_won = data.mission_id(side, n) in ctx.checked_locations
                won += is_won and n < data.FINAL_MISSION
                self.tiles[(side, n)].show(n in unlocked, is_won, done, len(ids), in_logic.get((side, n), 0),
                                           out_logic.get((side, n), 0), tracker, n == data.FINAL_MISSION)
            self.side_labels[side].text = (f"[b]{data.SIDES[side]}[/b]   [size=14dp][color=AAAAAA]{won} won"
                                           f"[/color][/size]")
        everything = ctx.missing_locations | ctx.checked_locations
        for title, low, high in (("Builds", data.HEAD_OFFSET, data.MISSION_OFFSET),
                                 ("Combos", data.COMBO_OFFSET, data.SHOP_OFFSET),
                                 ("Shop", data.SHOP_OFFSET, data.SPY_OFFSET)):
            ids = [i for i in everything if low <= i < high]
            if ids:
                parts.append(f"{title}: {sum(1 for i in ids if i in ctx.checked_locations)}/{len(ids)}")
        self.status.text = "    ".join(parts)
