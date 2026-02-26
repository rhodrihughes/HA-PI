/**
 * touch_driver.h — XPT2046 SPI touchscreen driver for LVGL 9.x
 *
 * Targets Raspberry Pi 3B+ with an XPT2046 resistive touchscreen
 * connected via SPI1 (CE1).
 * Uses ONLY LVGL 9.x APIs (no v8 functions).
 *
 * Hardware wiring:
 *   SPI device : /dev/spidev0.1 at 1 MHz
 *   IRQ (pen)  : GPIO 24 (active low, directly read for touch detect)
 *
 * Polling runs at 50 Hz in a dedicated background pthread.
 * Touch coordinates are mapped from raw ADC values to screen pixels
 * using calibration constants.
 *
 * Requirements: 2.1, 2.2, 2.3, 2.4
 */

#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include "lvgl.h"

/**
 * Initialise the XPT2046 touchscreen and register with LVGL 9.x.
 *
 * Opens /dev/spidev0.1 at 1 MHz, starts a 50 Hz polling thread,
 * and registers the input device via lv_indev_create() +
 * lv_indev_set_type(LV_INDEV_TYPE_POINTER).
 *
 * @return 0 on success, -1 on failure
 */
int touch_driver_init(void);

/**
 * De-initialise the touch driver.
 *
 * Stops the polling thread, closes the SPI file descriptor,
 * and cleans up resources.
 */
void touch_driver_deinit(void);

#endif /* TOUCH_DRIVER_H */
