#!/usr/bin/env bash
# HA Lights Controller — Pi Setup Script
# Run on a fresh Raspberry Pi OS Lite (32-bit Bookworm):
#   curl -sL https://raw.githubusercontent.com/rhodrihughes/ha-pi/main/setup.sh | bash
#
# What this does:
#   1. Installs build dependencies (gcc, libcurl, libcrypt, git)
#   2. Enables SPI if not already enabled
#   3. Clones the repo + LVGL + Mongoose
#   4. Builds the binary
#   5. Creates a starter config file
#   6. Installs and starts the systemd service

set -euo pipefail

REPO="https://github.com/rhodrihughes/ha-pi.git"
INSTALL_DIR="${HOME}/ha-pi"
CONFIG_PATH="/etc/ha_lights.conf"
LVGL_BRANCH="release/v9.2"
MONGOOSE_VERSION="7.16"

echo "==> HA Lights Controller Setup"
echo ""

# --- 1. System packages ---
echo "[1/6] Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq git build-essential libcurl4-openssl-dev libcrypt-dev

# --- 2. Enable SPI ---
echo "[2/6] Enabling SPI..."
if ! grep -q "^dtparam=spi=on" /boot/firmware/config.txt 2>/dev/null && \
   ! grep -q "^dtparam=spi=on" /boot/config.txt 2>/dev/null; then
    sudo raspi-config nonint do_spi 0
    echo "    SPI enabled (reboot needed after setup)"
    NEEDS_REBOOT=1
else
    echo "    SPI already enabled"
    NEEDS_REBOOT=0
fi

# Ensure user is in spi and gpio groups for hardware access
sudo usermod -aG spi,gpio "$(whoami)" 2>/dev/null || true

# --- 3. Clone repo + dependencies ---
echo "[3/6] Cloning repo and dependencies..."
if [ -d "$INSTALL_DIR" ]; then
    echo "    $INSTALL_DIR exists, pulling latest..."
    cd "$INSTALL_DIR"
    git pull --ff-only
else
    git clone "$REPO" "$INSTALL_DIR"
    cd "$INSTALL_DIR"
fi

# LVGL
if [ ! -d "lvgl/src" ]; then
    echo "    Cloning LVGL 9.x..."
    rm -rf lvgl
    git clone --branch "$LVGL_BRANCH" --depth 1 https://github.com/lvgl/lvgl.git lvgl
else
    echo "    LVGL already present"
fi

# Mongoose
if [ ! -f "src/mongoose.c" ]; then
    echo "    Downloading Mongoose ${MONGOOSE_VERSION}..."
    curl -sL -o src/mongoose.c "https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.c"
    curl -sL -o src/mongoose.h "https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.h"
else
    echo "    Mongoose already present"
fi

# --- 4. Build ---
echo "[4/6] Building..."
make clean
make -j"$(nproc)"
echo "    Built: ${INSTALL_DIR}/ha_lights"

# --- 5. Config file ---
if [ ! -f "$CONFIG_PATH" ]; then
    echo "[5/6] Creating starter config at ${CONFIG_PATH}..."
    echo "    You'll need to fill in your HA URL, token, and lights."
    sudo tee "$CONFIG_PATH" > /dev/null <<'CONF'
{
  "ha_url": "",
  "ha_token": "",
  "web_password_hash": "",
  "lights": []
}
CONF
    sudo chmod 600 "$CONFIG_PATH"
    sudo chown "$(whoami):$(whoami)" "$CONFIG_PATH"
else
    echo "[5/6] Config already exists at ${CONFIG_PATH}, skipping"
fi

# --- 6. Systemd service ---
echo "[6/6] Installing systemd service..."
sed "s|REPLACE_USER|$(whoami)|g; s|REPLACE_HOME|${HOME}|g" "${INSTALL_DIR}/ha-pi.service" | sudo tee /etc/systemd/system/ha-pi.service > /dev/null
sudo systemctl daemon-reload
sudo systemctl enable ha-pi

echo ""
echo "==> Setup complete!"
echo ""
echo "Next steps:"
echo "  1. Edit your config:  sudo nano ${CONFIG_PATH}"
echo "     - Set ha_url to your Home Assistant address"
echo "     - Set ha_token to a long-lived access token"
echo "     - Add your lights"
echo "  2. Generate a web password hash (optional):"
echo "     htpasswd -bnBC 10 \"\" yourpassword | tr -d ':\\n'"
echo "  3. Start the service:  sudo systemctl start ha-pi"
echo "  4. View logs:          journalctl -u ha-pi -f"

if [ "$NEEDS_REBOOT" -eq 1 ]; then
    echo ""
    echo "  ⚠  SPI was just enabled — reboot first:  sudo reboot"
fi
