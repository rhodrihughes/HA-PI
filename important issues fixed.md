Issues found and fixed:

Config crash on missing credentials — config.c required ha_url, ha_token, and lights array or the app exited. Now all three are optional; app starts with empty values and shows setup screen.

HA client crash on empty credentials — main.c exited if ha_client_init failed. Now non-fatal; UI shows without HA connection, web config at :8080 still works.

Setup screen — Added show_setup_screen() in light_ui.c that displays "Setup Required" with instructions when no lights are configured.

Sysfs GPIO deprecated on Bookworm — display_driver.c and touch_driver.c used /sys/class/gpio which doesn't work on Bookworm. Display driver rewritten to use Linux framebuffer (/dev/fb1). Touch driver rewritten to use Linux input subsystem (/dev/input/eventX).

SPI "Message too long" — Raw SPI transfers exceeded kernel's 4096-byte limit. Fixed by switching to framebuffer (kernel handles SPI).

Service file User=pi — Hardcoded pi user. Now uses REPLACE_USER/REPLACE_HOME placeholders, setup.sh substitutes actual values via sed.

fbcp overwriting framebuffer — LCD-show installs fbcp which copies fb0→fb1 continuously. Service file now kills fbcp before starting. Setup script removes fbcp from rc.local.

Build warnings — Fixed lv_color16_t.full error (LVGL 9.x), config_server.c format-truncation warnings, light_ui.c unused parameter warning.

Montserrat 16 font — Enabled in lv_conf.h for setup screen body text.