/**
 * lv_conf.h — Minimal LVGL 9.x configuration
 *
 * Targets: Raspberry Pi 3B+ with ILI9486 480×320 SPI display.
 * Only memory, colour depth, fonts, and tick config are set here.
 * No v8-era widget enable flags or LV_HOR_RES_MAX / LV_VER_RES_MAX.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Colour depth: 16-bit (RGB565) matches ILI9486 native format */
#define LV_COLOR_DEPTH 16

/* Memory pool for LVGL internal allocations */
#define LV_MEM_SIZE (128 * 1024)

/* Fonts — Montserrat 16 for body text, 24 for labels, 32 for icons / headings */
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1

/* Tick source — use clock_gettime directly so LVGL can call it from
 * any compilation unit without depending on a user-defined function. */
#include <time.h>
static inline uint32_t lv_tick_get_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (lv_tick_get_ms())

#endif /* LV_CONF_H */
