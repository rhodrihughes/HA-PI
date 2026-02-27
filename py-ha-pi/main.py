#!/usr/bin/env python3
"""
HA Lights Controller — Python/tkinter version.

Touchscreen app for Raspberry Pi that controls Home Assistant lights.
Tap tiles to toggle, swipe to page. Web config at :8080.
"""

import logging
import os
import signal
import sys
import tkinter as tk

import config
import config_server
import ha_client
from light_ui import LightUI

# Ensure we target the right X display — LCD-show typically uses :0
# If running via SSH or systemd, DISPLAY may not be set
if "DISPLAY" not in os.environ:
    os.environ["DISPLAY"] = ":0"

# Point to the X authority file so we can connect when running as a service
if "XAUTHORITY" not in os.environ:
    # Common locations on Pi OS
    for xauth in [
        os.path.expanduser("~/.Xauthority"),
        "/home/pi/.Xauthority",
    ]:
        if os.path.exists(xauth):
            os.environ["XAUTHORITY"] = xauth
            break

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("main")

DEFAULT_CONFIG_PATH = "/etc/ha_lights.conf"
WEB_PORT = 8080
POLL_INTERVAL_MS = 5000


class App:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("HA Lights")
        self.ui: LightUI | None = None
        self.cfg: dict = {}
        self.ha_ok = False

    def run(self, config_path: str):
        # Load config
        try:
            self.cfg = config.load(config_path)
        except Exception as e:
            log.error("Failed to load config: %s", e)
            self.cfg = {"ha_url": "", "ha_token": "", "web_password": "", "lights": []}

        config.set_path(config_path)

        # Fullscreen on Pi — works with LCD-show's X11 setup
        try:
            self.root.attributes("-fullscreen", True)
        except tk.TclError:
            pass

        # Override window manager to stay on top and remove decorations
        self.root.overrideredirect(True)
        self.root.configure(cursor="none")  # hide cursor for touchscreen

        # Force geometry to match the SPI display
        self.root.geometry("480x320+0+0")

        # Build UI
        self.ui = LightUI(self.root, self.cfg.get("lights", []),
                          toggle_cb=self._on_toggle)

        # Init HA client
        url = self.cfg.get("ha_url", "")
        token = self.cfg.get("ha_token", "")
        if url and token:
            ha_client.init(url, token)
            self.ha_ok = True
            self._poll_states()
        else:
            log.warning("HA credentials not configured — use web config at :%d", WEB_PORT)

        # Start web config server
        try:
            config_server.start(WEB_PORT, self.cfg, reload_cb=self._on_config_reload)
        except Exception as e:
            log.error("Config server failed (non-fatal): %s", e)

        # Schedule polling
        if self.ha_ok:
            self.root.after(POLL_INTERVAL_MS, self._poll_loop)

        # Handle Ctrl+C
        signal.signal(signal.SIGINT, lambda *_: self._shutdown())
        signal.signal(signal.SIGTERM, lambda *_: self._shutdown())

        # Allow signal handling in mainloop
        self._check_signals()

        log.info("ha-pi: running (config=%s)", config_path)
        self.root.mainloop()

    def _check_signals(self):
        """Periodically yield to allow signal handlers to fire."""
        self.root.after(500, self._check_signals)

    def _on_toggle(self, entity_id: str, current_state: str):
        """Called when user taps a tile."""
        if self.ha_ok:
            # Fire toggle in background to avoid blocking UI
            self.root.after(1, lambda: ha_client.toggle_light(entity_id))

    def _poll_states(self):
        """Fetch all light states and update UI."""
        if not self.ha_ok or not self.ui:
            return
        lights = self.cfg.get("lights", [])
        states = ha_client.poll_all(lights)
        for eid, state in states.items():
            if state in ("on", "off"):
                self.ui.set_state(eid, state)
            elif state == "unavailable":
                self.ui.set_state(eid, "unknown")

    def _poll_loop(self):
        """Periodic polling via tkinter's after()."""
        self._poll_states()
        self.root.after(POLL_INTERVAL_MS, self._poll_loop)

    def _on_config_reload(self):
        """Called by web config server after saving new config."""
        try:
            new_cfg = config.reload()
            if new_cfg:
                self.cfg = new_cfg
                # Re-init HA client if credentials changed
                url = self.cfg.get("ha_url", "")
                token = self.cfg.get("ha_token", "")
                if url and token:
                    ha_client.cleanup()
                    ha_client.init(url, token)
                    self.ha_ok = True
                # Rebuild UI on main thread
                self.root.after(0, lambda: self.ui.rebuild(self.cfg.get("lights", [])))
                log.info("Config reloaded successfully")
        except Exception as e:
            log.error("Config reload failed: %s", e)

    def _shutdown(self):
        log.info("Shutting down")
        ha_client.cleanup()
        self.root.quit()


def main():
    config_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CONFIG_PATH
    app = App()
    app.run(config_path)


if __name__ == "__main__":
    main()
