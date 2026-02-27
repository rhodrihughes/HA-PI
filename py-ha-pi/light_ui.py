"""Light tile grid UI using tkinter — 480x320 fullscreen on Pi framebuffer."""

import tkinter as tk
import logging
import math

log = logging.getLogger("light_ui")

# Layout
DISP_W, DISP_H = 480, 320
TILE_W, TILE_H = 220, 130
TILE_GAP = 10
OUTER_PAD = 10
TILE_RADIUS = 12
COLS, ROWS = 2, 2
PER_PAGE = COLS * ROWS
MAX_LIGHTS = 16
DOT_SIZE = 8
DOT_SPACING = 16
DOT_Y = DISP_H - 20

# Colours
COLOR_BG = "#1A1A2E"
STYLES = {
    "on":      {"bg": "#FFC864", "fg": "#1A1A2E", "icon": "#1A1A2E"},
    "off":     {"bg": "#2A2A3E", "fg": "#888899", "icon": "#555566"},
    "unknown": {"bg": "#3A3A5C", "fg": "#7777AA", "icon": "#6666AA"},
}


class LightUI:
    def __init__(self, root: tk.Tk, lights: list[dict], toggle_cb=None):
        self.root = root
        self.lights = lights[:MAX_LIGHTS]
        self.toggle_cb = toggle_cb
        self.page_count = max(1, math.ceil(len(self.lights) / PER_PAGE))
        self.current_page = 0

        # State tracking
        self.states = {l["entity_id"]: "unknown" for l in self.lights}
        self.optimistic = dict(self.states)

        # Root window setup
        self.root.configure(bg=COLOR_BG)
        self.root.geometry(f"{DISP_W}x{DISP_H}")

        # Main canvas
        self.canvas = tk.Canvas(
            root, width=DISP_W, height=DISP_H,
            bg=COLOR_BG, highlightthickness=0,
        )
        self.canvas.pack(fill="both", expand=True)

        # Tile storage: {entity_id: {rect, icon_text, name_text}}
        self.tile_items: dict[str, dict] = {}
        self.dot_items: list[int] = []

        # Swipe tracking
        self._swipe_start_x = None
        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)

        if not self.lights:
            self._show_setup_screen()
        else:
            self._build_tiles()
            self._build_dots()
            self._show_page(animate=False)

    # ------------------------------------------------------------------
    #  Setup screen (no lights configured)
    # ------------------------------------------------------------------

    def _show_setup_screen(self):
        cx, cy = DISP_W // 2, DISP_H // 2
        self.canvas.create_text(
            cx, cy - 40, text="⚙", font=("sans-serif", 32),
            fill="#FFC864",
        )
        self.canvas.create_text(
            cx, cy, text="Setup Required", font=("sans-serif", 20),
            fill="white",
        )
        self.canvas.create_text(
            cx, cy + 40,
            text="Open the web config to continue:\nhttp://<this-pi>:8080",
            font=("sans-serif", 12), fill="#888899", justify="center",
        )

    # ------------------------------------------------------------------
    #  Build tiles
    # ------------------------------------------------------------------

    def _build_tiles(self):
        for i, light in enumerate(self.lights):
            page = i // PER_PAGE
            slot = i % PER_PAGE
            col = slot % COLS
            row = slot // COLS

            # Position relative to page (page offset applied in _show_page)
            x = OUTER_PAD + col * (TILE_W + TILE_GAP)
            y = OUTER_PAD + row * (TILE_H + TILE_GAP)

            # Store base x plus page offset
            base_x = x + page * DISP_W

            style = STYLES["unknown"]

            # Rounded rect (approximated with oval-cornered polygon)
            rect = self._rounded_rect(
                base_x, y, base_x + TILE_W, y + TILE_H,
                TILE_RADIUS, fill=style["bg"], outline="",
            )

            icon_text = self.canvas.create_text(
                base_x + TILE_W // 2, y + 40,
                text=light.get("icon", "💡"), font=("sans-serif", 24),
                fill=style["icon"],
            )

            name_text = self.canvas.create_text(
                base_x + TILE_W // 2, y + 85,
                text=light["label"], font=("sans-serif", 16),
                fill=style["fg"], width=TILE_W - 20,
            )

            eid = light["entity_id"]
            self.tile_items[eid] = {
                "rect": rect, "icon_text": icon_text,
                "name_text": name_text, "base_x": base_x,
                "y": y, "page": page,
            }

            # Bind click on all tile elements
            for item in (rect, icon_text, name_text):
                self.canvas.tag_bind(item, "<Button-1>",
                                     lambda e, _eid=eid: self._on_tile_click(_eid))

    def _rounded_rect(self, x1, y1, x2, y2, r, **kwargs):
        """Draw a rounded rectangle on the canvas."""
        points = [
            x1 + r, y1, x2 - r, y1,
            x2, y1, x2, y1 + r,
            x2, y2 - r, x2, y2,
            x2 - r, y2, x1 + r, y2,
            x1, y2, x1, y2 - r,
            x1, y1 + r, x1, y1,
        ]
        return self.canvas.create_polygon(points, smooth=True, **kwargs)

    # ------------------------------------------------------------------
    #  Page dots
    # ------------------------------------------------------------------

    def _build_dots(self):
        if self.page_count <= 1:
            return
        total_w = self.page_count * DOT_SIZE + (self.page_count - 1) * (DOT_SPACING - DOT_SIZE)
        start_x = (DISP_W - total_w) // 2
        for i in range(self.page_count):
            x = start_x + i * DOT_SPACING
            dot = self.canvas.create_oval(
                x, DOT_Y, x + DOT_SIZE, DOT_Y + DOT_SIZE,
                fill="white", outline="",
            )
            self.dot_items.append(dot)

    def _update_dots(self):
        for i, dot in enumerate(self.dot_items):
            opacity_fill = "white" if i == self.current_page else "#555566"
            self.canvas.itemconfigure(dot, fill=opacity_fill)

    # ------------------------------------------------------------------
    #  Page navigation
    # ------------------------------------------------------------------

    def _show_page(self, animate=True):
        offset = -self.current_page * DISP_W
        for eid, items in self.tile_items.items():
            page = items["page"]
            slot_in_page = list(self.tile_items.keys()).index(eid) % PER_PAGE
            col = slot_in_page % COLS
            row = slot_in_page // COLS
            target_x = OUTER_PAD + col * (TILE_W + TILE_GAP) + page * DISP_W + offset
            y = items["y"]

            # Move tile elements
            self._move_tile(eid, target_x, y)

        self._update_dots()

    def _move_tile(self, eid: str, x: int, y: int):
        items = self.tile_items[eid]
        # Delete old rect and redraw at new position
        self.canvas.delete(items["rect"])
        style_key = self.optimistic.get(eid, "unknown")
        if style_key not in STYLES:
            style_key = "unknown"
        style = STYLES[style_key]

        rect = self._rounded_rect(
            x, y, x + TILE_W, y + TILE_H,
            TILE_RADIUS, fill=style["bg"], outline="",
        )
        self.canvas.coords(items["icon_text"], x + TILE_W // 2, y + 40)
        self.canvas.coords(items["name_text"], x + TILE_W // 2, y + 85)

        items["rect"] = rect
        # Re-bind click
        for item in (rect, items["icon_text"], items["name_text"]):
            self.canvas.tag_bind(item, "<Button-1>",
                                 lambda e, _eid=eid: self._on_tile_click(_eid))

        # Ensure text is above rect
        self.canvas.tag_raise(items["icon_text"])
        self.canvas.tag_raise(items["name_text"])

        # Keep dots on top
        for dot in self.dot_items:
            self.canvas.tag_raise(dot)

    # ------------------------------------------------------------------
    #  Swipe handling
    # ------------------------------------------------------------------

    def _on_press(self, event):
        self._swipe_start_x = event.x

    def _on_release(self, event):
        if self._swipe_start_x is None:
            return
        dx = event.x - self._swipe_start_x
        self._swipe_start_x = None

        if abs(dx) < 50:
            return  # Not a swipe, just a tap

        if dx < 0 and self.current_page < self.page_count - 1:
            self.current_page += 1
            self._show_page()
        elif dx > 0 and self.current_page > 0:
            self.current_page -= 1
            self._show_page()

    # ------------------------------------------------------------------
    #  Tile interaction
    # ------------------------------------------------------------------

    def _on_tile_click(self, entity_id: str):
        current = self.optimistic.get(entity_id, "unknown")
        # Optimistic toggle
        if current == "on":
            nxt = "off"
        else:
            nxt = "on"

        self.optimistic[entity_id] = nxt
        self._apply_style(entity_id)

        if self.toggle_cb:
            self.toggle_cb(entity_id, current)

    def _apply_style(self, entity_id: str):
        if entity_id not in self.tile_items:
            return
        items = self.tile_items[entity_id]
        state = self.optimistic.get(entity_id, "unknown")
        if state not in STYLES:
            state = "unknown"
        style = STYLES[state]

        self.canvas.itemconfigure(items["rect"], fill=style["bg"])
        self.canvas.itemconfigure(items["icon_text"], fill=style["icon"])
        self.canvas.itemconfigure(items["name_text"], fill=style["fg"])

    # ------------------------------------------------------------------
    #  Public API
    # ------------------------------------------------------------------

    def set_state(self, entity_id: str, state: str):
        """Update confirmed + optimistic state and restyle."""
        if entity_id not in self.states:
            return
        self.states[entity_id] = state
        self.optimistic[entity_id] = state
        self._apply_style(entity_id)

    def destroy(self):
        self.canvas.delete("all")
        self.tile_items.clear()
        self.dot_items.clear()

    def rebuild(self, lights: list[dict]):
        """Tear down and rebuild with new light list."""
        self.destroy()
        self.lights = lights[:MAX_LIGHTS]
        self.page_count = max(1, math.ceil(len(self.lights) / PER_PAGE))
        self.current_page = 0
        self.states = {l["entity_id"]: "unknown" for l in self.lights}
        self.optimistic = dict(self.states)
        self.tile_items = {}
        self.dot_items = []

        if not self.lights:
            self._show_setup_screen()
        else:
            self._build_tiles()
            self._build_dots()
            self._show_page(animate=False)
