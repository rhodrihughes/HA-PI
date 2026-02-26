# HA Lights Controller

A touchscreen app for Raspberry Pi 3B+ that controls Home Assistant lights. Built with C and LVGL 9.x, it drives a 480×320 ILI9486 SPI display with an XPT2046 resistive touchscreen.

Tap tiles to toggle lights. Swipe to page through them. Edit the config from any browser on your network.

## Hardware

- Raspberry Pi 3B+
- ILI9486 480×320 SPI display on `/dev/spidev0.0` (DC=GPIO25, RST=GPIO27, BL=GPIO18)
- XPT2046 resistive touchscreen on `/dev/spidev0.1` (IRQ=GPIO24)

## Quick Setup

SSH into your Pi and run:

```bash
curl -sL https://raw.githubusercontent.com/rhodrihughes/ha-pi/main/setup.sh | bash
```

This installs dependencies, enables SPI, clones the repo, pulls in LVGL and Mongoose, builds the binary, creates a starter config, and installs the systemd service. After it finishes, edit `/etc/ha_lights.conf` with your HA details and start the service.

## Recommended OS

Use Raspberry Pi OS Lite (32-bit, Debian Bookworm). The Lite image has no desktop environment, which is exactly what you want — this app drives the display directly via SPI and doesn't need X11 or Wayland.

Download it from https://www.raspberrypi.com/software/operating-systems/ or flash it with Raspberry Pi Imager.

After flashing, enable SPI:

```bash
sudo raspi-config nonint do_spi 0
```

The 32-bit (armhf) build is the best fit for the Pi 3B+'s 1GB RAM. The 64-bit image works too but offers no real advantage here and uses slightly more memory.

## Dependencies

Install on the Pi (or in your cross-compilation sysroot):

```bash
sudo apt install libcurl4-openssl-dev libcrypt-dev
```

Then pull in the two vendored C libraries that get compiled into the binary:

LVGL (the graphics library that renders the UI):

```bash
sudo git clone --branch release/v9.2 --depth 1 https://github.com/lvgl/lvgl.git lvgl
```

Mongoose (a single-file embedded HTTP server for the web config UI):

```bash
curl -L -o src/mongoose.c https://raw.githubusercontent.com/cesanta/mongoose/7.16/mongoose.c
curl -L -o src/mongoose.h https://raw.githubusercontent.com/cesanta/mongoose/7.16/mongoose.h
```

Neither of these are system packages — they're source files that live in the repo and get compiled alongside the app code.

## Configuration

Create `/etc/ha_lights.conf`:

```json
{
  "ha_url": "http://192.168.1.100:8123",
  "ha_token": "your-long-lived-access-token",
  "web_password_hash": "$2b$10$...",
  "lights": [
    { "entity_id": "light.living_room", "label": "Living Room", "icon": "bulb" },
    { "entity_id": "light.kitchen",     "label": "Kitchen",     "icon": "bulb" }
  ]
}
```

To generate the web password hash:

```bash
htpasswd -bnBC 10 "" yourpassword | tr -d ':\n'
```

Lock down the file:

```bash
sudo chmod 600 /etc/ha_lights.conf
sudo chown pi:pi /etc/ha_lights.conf
```

### Getting a Home Assistant Token

1. Open your HA instance in a browser
2. Click your profile (bottom-left)
3. Scroll to "Long-Lived Access Tokens"
4. Create a token and paste it into `ha_token`

## Build

Build natively on the Pi:

```bash
make
```

Cross-compile from another machine:

```bash
make CC=arm-linux-gnueabihf-gcc
```

## Deploy

Push the binary to the Pi and restart the service in one step:

```bash
make deploy PI_HOST=pi@192.168.1.50
```

This copies the binary to `~/ha-pi/`, installs the systemd unit, and restarts the service. Make sure the destination directory exists:

```bash
ssh pi@192.168.1.50 "mkdir -p ~/ha-pi"
```

## Run

Manually:

```bash
./ha_lights                        # uses /etc/ha_lights.conf
./ha_lights ./my_config.json       # custom config path
```

As a systemd service:

```bash
sudo systemctl enable ha-pi
sudo systemctl start ha-pi
```

Check logs:

```bash
journalctl -u ha-pi -f
```

## Web Configuration

Once running, open `http://<pi-ip>:8080` in a browser to manage lights without SSH. Log in with the password matching your bcrypt hash, then add/remove/reorder lights and update HA connection settings. Changes take effect immediately on the display.

## Usage

- Tap a tile to toggle a light (instant visual feedback, confirmed within 5 seconds)
- Swipe left/right to navigate pages (4 lights per page, up to 16 total)
- Dots at the bottom show which page you're on

## Project Structure

```
├── include/           Header files
│   ├── config.h
│   ├── config_server.h
│   ├── display_driver.h
│   ├── ha_client.h
│   ├── light_ui.h
│   └── touch_driver.h
├── src/               Implementation
│   ├── main.c
│   ├── config.c
│   ├── config_server.c
│   ├── display_driver.c
│   ├── ha_client.c
│   ├── light_ui.c
│   └── touch_driver.c
├── lvgl/              LVGL 9.x source (git submodule or copy)
├── lv_conf.h          Minimal LVGL config
├── ha-pi.service      systemd unit file
└── Makefile
```
