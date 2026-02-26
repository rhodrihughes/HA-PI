/**
 * touch_driver.h — Touchscreen driver for LVGL 9.x
 *
 * Reads touch events from the Linux input subsystem (/dev/input/eventX).
 * Auto-detects the touchscreen device by scanning for ABS_X capability.
 * Works with kernel-managed touch controllers (e.g. XPT2046 via fbtft/LCD-show).
 *
 * Uses ONLY LVGL 9.x APIs (no v8 functions).
 *
 * Requirements: 2.1, 2.2, 2.3, 2.4
 */

#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include "lvgl.h"

/**
 * Initialise the touchscreen and register with LVGL 9.x.
 *
 * Scans /dev/input/event* for a device with ABS_X capability,
 * starts an event reading thread, and registers the input device
 * via lv_indev_create() + lv_indev_set_type(LV_INDEV_TYPE_POINTER).
 *
 * @return 0 on success, -1 on failure
 */
int touch_driver_init(void);

/**
 * De-initialise the touch driver.
 *
 * Stops the event reading thread and closes the input device fd.
 */
void touch_driver_deinit(void);

#endif /* TOUCH_DRIVER_H */
