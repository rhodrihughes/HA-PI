/**
 * display_driver.c — Display driver for LVGL 9.x
 *
 * Supports Linux framebuffer devices (e.g. /dev/fb0, /dev/fb1) which
 * is the standard way to drive goodtft/waveshare-style SPI displays
 * on Raspberry Pi after installing their kernel overlay (LCD-show).
 *
 * The kernel's fbtft driver handles all SPI communication, GPIO control,
 * and display initialisation. We simply mmap the framebuffer and copy
 * LVGL's rendered pixels into it.
 *
 * Framebuffer search order: /dev/fb1, /dev/fb0
 * (fb1 is typical for SPI displays when HDMI is fb0)
 *
 * Requirements: 1.1, 1.2, 1.3, 1.4
 */

#include "display_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                     */
/* ------------------------------------------------------------------ */

/* Draw buffer: 10 lines at a time */
#define DRAW_BUF_LINES 10
#define DRAW_BUF_SIZE  (DISP_HOR_RES * DRAW_BUF_LINES * sizeof(lv_color16_t))

/* ------------------------------------------------------------------ */
/*  Module-level state                                                */
/* ------------------------------------------------------------------ */

static int fb_fd = -1;               /* Framebuffer file descriptor   */
static uint8_t *fb_map = NULL;       /* mmap'd framebuffer memory     */
static size_t fb_size = 0;           /* Total framebuffer size        */
static uint32_t fb_line_length = 0;  /* Bytes per scanline            */
static uint32_t fb_bpp = 16;         /* Bits per pixel                */

static lv_display_t *disp = NULL;    /* LVGL display handle           */
static uint8_t *draw_buf = NULL;     /* LVGL draw buffer              */

/* ------------------------------------------------------------------ */
/*  Framebuffer helpers                                               */
/* ------------------------------------------------------------------ */

/**
 * Try to open a framebuffer device and mmap it.
 * Returns 0 on success, -1 on failure.
 */
static int fb_open(const char *dev)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    fb_fd = open(dev, O_RDWR);
    if (fb_fd < 0)
        return -1;

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    fb_bpp = vinfo.bits_per_pixel;
    fb_line_length = finfo.line_length;
    fb_size = (size_t)finfo.smem_len;

    fprintf(stderr, "display_driver: %s — %dx%d, %d bpp, line_length=%u\n",
            dev, vinfo.xres, vinfo.yres, fb_bpp, fb_line_length);

    /* mmap the framebuffer */
    fb_map = (uint8_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED) {
        fprintf(stderr, "display_driver: mmap failed: %s\n", strerror(errno));
        fb_map = NULL;
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    /* Clear to black */
    memset(fb_map, 0, fb_size);

    /* Blank the framebuffer console cursor by hiding it */
    int blank = 1;
    ioctl(fb_fd, FBIOBLANK, blank);
    ioctl(fb_fd, FBIOBLANK, 0);  /* unblank so display stays on */

    return 0;
}

/* ------------------------------------------------------------------ */
/*  LVGL flush callback                                               */
/* ------------------------------------------------------------------ */

/**
 * LVGL 9.x flush callback — framebuffer version.
 *
 * Copies rendered pixels from LVGL's draw buffer directly into the
 * mmap'd framebuffer memory. Handles both 16-bit (RGB565) and 32-bit
 * (ARGB8888) framebuffers.
 */
static void disp_flush_cb(lv_display_t *display, const lv_area_t *area,
                           uint8_t *px_map)
{
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;
    int32_t w  = x2 - x1 + 1;

    if (fb_map && fb_bpp == 16) {
        /* 16-bit RGB565 — direct copy, line by line */
        uint32_t src_stride = (uint32_t)(w * 2);
        for (int32_t y = y1; y <= y2; y++) {
            uint32_t fb_offset = (uint32_t)y * fb_line_length + (uint32_t)x1 * 2;
            uint32_t src_offset = (uint32_t)(y - y1) * src_stride;
            memcpy(fb_map + fb_offset, px_map + src_offset, src_stride);
        }
    } else if (fb_map && fb_bpp == 32) {
        /* 32-bit ARGB — convert from RGB565 */
        uint16_t *src = (uint16_t *)px_map;
        for (int32_t y = y1; y <= y2; y++) {
            uint32_t *dst = (uint32_t *)(fb_map + (uint32_t)y * fb_line_length
                            + (uint32_t)x1 * 4);
            for (int32_t x = 0; x < w; x++) {
                uint16_t c = src[(y - y1) * w + x];
                uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3);
                uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) << 2);
                uint8_t b = (uint8_t)((c & 0x1F) << 3);
                dst[x] = (0xFF000000u) | ((uint32_t)r << 16)
                          | ((uint32_t)g << 8) | b;
            }
        }
    }

    lv_display_flush_ready(display);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int display_driver_init(void)
{
    /* Try framebuffer devices in order of preference */
    const char *fb_devices[] = { "/dev/fb1", "/dev/fb0", NULL };

    for (int i = 0; fb_devices[i]; i++) {
        if (fb_open(fb_devices[i]) == 0) {
            fprintf(stderr, "display_driver_init: using %s\n", fb_devices[i]);
            break;
        }
    }

    if (fb_fd < 0) {
        fprintf(stderr, "display_driver_init: no framebuffer found\n");
        return -1;
    }

    /* --- LVGL display registration (9.x API only) ----------------- */

    draw_buf = (uint8_t *)malloc(DRAW_BUF_SIZE);
    if (!draw_buf) {
        fprintf(stderr, "display_driver_init: draw buffer alloc failed\n");
        display_driver_deinit();
        return -1;
    }

    disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    if (!disp) {
        fprintf(stderr, "display_driver_init: lv_display_create failed\n");
        display_driver_deinit();
        return -1;
    }

    lv_display_set_buffers(disp, draw_buf, NULL, DRAW_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, disp_flush_cb);

    fprintf(stderr, "display_driver_init: %dx%d framebuffer ready\n",
            DISP_HOR_RES, DISP_VER_RES);
    return 0;
}

void display_driver_deinit(void)
{
    if (fb_map) {
        munmap(fb_map, fb_size);
        fb_map = NULL;
    }

    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }

    if (draw_buf) {
        free(draw_buf);
        draw_buf = NULL;
    }

    disp = NULL;
}
