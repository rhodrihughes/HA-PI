/**
 * display_driver.h — ILI9486 SPI display driver for LVGL 9.x
 *
 * Targets Raspberry Pi 3B+ with a 480×320 ILI9486 SPI display.
 * Uses ONLY LVGL 9.x APIs (no v8 functions).
 *
 * Hardware wiring:
 *   SPI device : /dev/spidev0.0 at 24 MHz
 *   DC  (data/command) : GPIO 25
 *   RST (reset)        : GPIO 27
 *   BL  (backlight)    : GPIO 18
 *
 * Requirements: 1.1, 1.2, 1.3, 1.4
 */

#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include "lvgl.h"

/** Screen dimensions */
#define DISP_HOR_RES 480
#define DISP_VER_RES 320

/**
 * Initialise the ILI9486 display hardware and register with LVGL 9.x.
 *
 * Opens /dev/spidev0.0 at 24 MHz, configures GPIO pins DC=25, RST=27, BL=18,
 * runs the ILI9486 init sequence, and registers the display with LVGL via
 * lv_display_create() + lv_display_set_flush_cb().
 *
 * @return 0 on success, -1 on failure
 */
int display_driver_init(void);

/**
 * De-initialise the display driver.
 *
 * Closes the SPI file descriptor, unexports GPIO pins, and turns off
 * the backlight.
 */
void display_driver_deinit(void);

#endif /* DISPLAY_DRIVER_H */
