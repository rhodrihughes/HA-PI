# HA Lights Controller

A touchscreen app for Raspberry Pi 3B+ that controls Home Assistant lights. Built with C and LVGL 9.x, it drives a 480×320 ILI9486 SPI display with an XPT2046 resistive touchscreen.

Tap tiles to toggle lights. Swipe to page through them. Edit the config from any browser on your network.

## Hardware

- Raspberry Pi 3B+
- 3.5" 480×320 SPI display (goodtft/waveshare ILI9486 style)
- XPT2046 resistive touchscreen (typically built into the display)
- Display driven via Linux framebuffer (kernel fbtft driver via LCD-show)

## Quick Setup

SSH into your Pi and run:

```bash
curl -sL https://raw.githubusercontent.com/rhodrihughes/ha-pi/main/setup.sh | bash
```

Retrying? Remove older versions first.

```bash
sudo systemctl stop ha-pi
rm -rf ~/ha-pi
sudo rm /etc/ha_lights.conf
sudo curl -sL https://raw.githubusercontent.com/rhodrihughes/ha-pi/main/setup.sh | bash

```

This installs dependencies, enables SPI, clones the repo, pulls in LVGL and Mongoose, builds the binary, creates a starter config, and installs the systemd service. After it finishes, edit `/etc/ha_lights.conf` with your HA details and start the service.

## Recommended OS

Use Raspberry Pi OS (32-bit, Debian Bookworm) with Desktop. The Desktop image is required for the LCD-show display driver which sets up the framebuffer. This app renders to the framebuffer directly via LVGL — it doesn't use the desktop environment itself.

Download it from https://www.raspberrypi.com/software/operating-systems/ or flash it with Raspberry Pi Imager.

## Display Setup

If you're using a goodtft/waveshare-style 3.5" SPI display, install the LCD-show driver before running the setup script:

```bash
cd ~
sudo rm -rf LCD-show
git clone https://github.com/goodtft/LCD-show.git
chmod -R 755 LCD-show
cd LCD-show
sudo ./LCD35-show
```

This reboots the Pi. After reboot, verify the framebuffer exists:

```bash
ls /dev/fb*
```

You should see `/dev/fb0` or `/dev/fb1`. The ha-pi display driver auto-detects the framebuffer.

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
