# HA Lights Controller (Python)

Python/tkinter rewrite of the C/LVGL touchscreen controller for Home Assistant lights on Raspberry Pi.

## Setup

```bash
cd py-ha-pi
pip install -r requirements.txt
```

tkinter comes with Python on Raspberry Pi OS. If missing:

```bash
sudo apt install python3-tk
```

## Run

```bash
cd ~/py-ha-pi
python3 main.py                        # uses /etc/ha_lights.conf
python3 main.py ./my_config.json       # custom config path
```

## SPI Display (LCD-show)

This works with the same LCD-show driver setup as the C version. LCD-show configures X11 to render to the SPI framebuffer, and tkinter renders through X11. Make sure:

1. LCD-show is installed and the display is working (`ls /dev/fb*`)
2. You're running Raspberry Pi OS with Desktop
3. The desktop is running (the app renders through X11, not directly to the framebuffer)

If running via SSH, set `DISPLAY=:0` (the app does this automatically).

## As a systemd service

```bash
sudo cp ha-pi.service /etc/systemd/system/
sudo systemctl enable ha-pi
sudo systemctl start ha-pi
```

The service waits for the graphical target and sets `DISPLAY=:0` + `XAUTHORITY` so tkinter can connect to X11.

## Config

Same JSON format as the C version — see the root README.

## Web Config

Open `http://<pi-ip>:8080` to manage lights from a browser.
