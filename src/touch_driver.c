/**
 * touch_driver.c — XPT2046 SPI touchscreen driver for LVGL 9.x
 *
 * Clean LVGL 9.x implementation from scratch. No v8 API usage.
 *
 * Hardware:
 *   SPI  : /dev/spidev0.1 at 1 MHz
 *   IRQ  : GPIO 24 (active low — low when pen is down)
 *
 * GPIO access uses libgpiod (character device interface) which is the
 * standard on Raspberry Pi OS Bookworm.
 *
 * The XPT2046 is a 12-bit resistive touch controller. Raw ADC values
 * are mapped to screen coordinates using calibration constants that
 * account for the specific panel mounting orientation.
 *
 * A background pthread polls the controller at 50 Hz and stores the
 * latest touch state behind a mutex. LVGL's read callback picks up
 * the state on each input tick.
 *
 * Requirements: 2.1, 2.2, 2.3, 2.4
 */

#include "touch_driver.h"
#include "display_driver.h"   /* DISP_HOR_RES, DISP_VER_RES */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>

/* ------------------------------------------------------------------ */
/*  Hardware / SPI configuration                                      */
/* ------------------------------------------------------------------ */

#define TOUCH_SPI_DEVICE   "/dev/spidev0.1"
#define TOUCH_SPI_SPEED_HZ 1000000   /* 1 MHz */
#define TOUCH_SPI_MODE     SPI_MODE_0
#define TOUCH_SPI_BPW      8         /* bits per word */

#define GPIO_CHIP       "gpiochip0"
#define GPIO_TOUCH_IRQ  24  /* Active low — low when pen is down */

/* ------------------------------------------------------------------ */
/*  XPT2046 command bytes                                             */
/* ------------------------------------------------------------------ */

#define XPT2046_CMD_X  0xD0  /* S=1, A2A1A0=101 (X), 12-bit, DFR, PD=00 */
#define XPT2046_CMD_Y  0x90  /* S=1, A2A1A0=001 (Y), 12-bit, DFR, PD=00 */

/* ------------------------------------------------------------------ */
/*  Calibration constants                                             */
/* ------------------------------------------------------------------ */

#define TOUCH_X_MIN  200
#define TOUCH_X_MAX  3900
#define TOUCH_Y_MIN  200
#define TOUCH_Y_MAX  3900

/* Number of samples to average for noise rejection */
#define TOUCH_SAMPLES 4

/* ------------------------------------------------------------------ */
/*  Polling configuration                                             */
/* ------------------------------------------------------------------ */

#define TOUCH_POLL_HZ       50
#define TOUCH_POLL_INTERVAL_US (1000000 / TOUCH_POLL_HZ)  /* 20 ms */

/* ------------------------------------------------------------------ */
/*  Shared touch state (protected by mutex)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t  x;
    int16_t  y;
    bool     pressed;
} touch_state_t;

static pthread_mutex_t touch_mutex = PTHREAD_MUTEX_INITIALIZER;
static touch_state_t   touch_state = { .x = 0, .y = 0, .pressed = false };

/* ------------------------------------------------------------------ */
/*  Module-level state                                                */
/* ------------------------------------------------------------------ */

static int spi_fd = -1;
static struct gpiod_chip *gpio_chip_touch = NULL;
static struct gpiod_line *line_irq = NULL;
static lv_indev_t *indev = NULL;

static pthread_t poll_thread;
static volatile bool poll_running = false;

/* ------------------------------------------------------------------ */
/*  SPI helpers                                                       */
/* ------------------------------------------------------------------ */

static int touch_spi_init(void)
{
    spi_fd = open(TOUCH_SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        fprintf(stderr, "touch_spi_init: cannot open %s: %s\n",
                TOUCH_SPI_DEVICE, strerror(errno));
        return -1;
    }

    uint8_t mode  = TOUCH_SPI_MODE;
    uint8_t bpw   = TOUCH_SPI_BPW;
    uint32_t speed = TOUCH_SPI_SPEED_HZ;

    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("touch_spi_init: SPI_IOC_WR_MODE");
        return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0) {
        perror("touch_spi_init: SPI_IOC_WR_BITS_PER_WORD");
        return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("touch_spi_init: SPI_IOC_WR_MAX_SPEED_HZ");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  XPT2046 reading                                                   */
/* ------------------------------------------------------------------ */

static uint16_t xpt2046_read_channel(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };

    struct spi_ioc_transfer tr = {
        .tx_buf        = (unsigned long)tx,
        .rx_buf        = (unsigned long)rx,
        .len           = 3,
        .speed_hz      = TOUCH_SPI_SPEED_HZ,
        .bits_per_word = TOUCH_SPI_BPW,
        .delay_usecs   = 0,
    };

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        perror("xpt2046_read_channel");
        return 0;
    }

    uint16_t raw = (uint16_t)((rx[1] << 8) | rx[2]) >> 3;
    return raw & 0x0FFF;
}

static void xpt2046_read_xy(uint16_t *raw_x, uint16_t *raw_y)
{
    uint32_t sum_x = 0;
    uint32_t sum_y = 0;

    /* Discard first reading for settling */
    (void)xpt2046_read_channel(XPT2046_CMD_X);
    (void)xpt2046_read_channel(XPT2046_CMD_Y);

    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        sum_x += xpt2046_read_channel(XPT2046_CMD_X);
        sum_y += xpt2046_read_channel(XPT2046_CMD_Y);
    }

    *raw_x = (uint16_t)(sum_x / TOUCH_SAMPLES);
    *raw_y = (uint16_t)(sum_y / TOUCH_SAMPLES);
}

static int16_t map_raw_to_screen(uint16_t raw, uint16_t raw_min,
                                  uint16_t raw_max, int16_t max_px)
{
    if (raw <= raw_min) return 0;
    if (raw >= raw_max) return max_px - 1;

    int32_t mapped = (int32_t)(raw - raw_min) * max_px / (raw_max - raw_min);

    if (mapped < 0) return 0;
    if (mapped >= max_px) return max_px - 1;
    return (int16_t)mapped;
}

/* ------------------------------------------------------------------ */
/*  Background polling thread                                         */
/* ------------------------------------------------------------------ */

static void *touch_poll_thread(void *arg)
{
    (void)arg;

    while (poll_running) {
        bool pressed = false;
        int16_t sx = 0;
        int16_t sy = 0;

        /* Check IRQ pin: active low means pen is down */
        bool pen_down = true;
        if (line_irq) {
            pen_down = (gpiod_line_get_value(line_irq) == 0);
        }

        if (pen_down) {
            uint16_t raw_x, raw_y;
            xpt2046_read_xy(&raw_x, &raw_y);

            if (raw_x > TOUCH_X_MIN && raw_x < TOUCH_X_MAX &&
                raw_y > TOUCH_Y_MIN && raw_y < TOUCH_Y_MAX) {
                sx = map_raw_to_screen(raw_x, TOUCH_X_MIN, TOUCH_X_MAX,
                                       DISP_HOR_RES);
                sy = map_raw_to_screen(raw_y, TOUCH_Y_MIN, TOUCH_Y_MAX,
                                       DISP_VER_RES);
                pressed = true;
            }
        }

        pthread_mutex_lock(&touch_mutex);
        touch_state.pressed = pressed;
        if (pressed) {
            touch_state.x = sx;
            touch_state.y = sy;
        }
        pthread_mutex_unlock(&touch_mutex);

        usleep(TOUCH_POLL_INTERVAL_US);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  LVGL input device read callback                                   */
/* ------------------------------------------------------------------ */

static void touch_read_cb(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;

    pthread_mutex_lock(&touch_mutex);
    data->point.x = touch_state.x;
    data->point.y = touch_state.y;
    data->state   = touch_state.pressed ? LV_INDEV_STATE_PRESSED
                                        : LV_INDEV_STATE_RELEASED;
    pthread_mutex_unlock(&touch_mutex);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int touch_driver_init(void)
{
    /* --- SPI setup ------------------------------------------------ */
    if (touch_spi_init() != 0) {
        fprintf(stderr, "touch_driver_init: SPI init failed\n");
        return -1;
    }

    /* --- GPIO IRQ pin (optional — graceful fallback) -------------- */
    gpio_chip_touch = gpiod_chip_open_by_name(GPIO_CHIP);
    if (gpio_chip_touch) {
        line_irq = gpiod_chip_get_line(gpio_chip_touch, GPIO_TOUCH_IRQ);
        if (line_irq) {
            if (gpiod_line_request_input(line_irq, "ha-pi-touch-irq") < 0) {
                fprintf(stderr, "touch_driver_init: cannot request IRQ GPIO input: %s\n",
                        strerror(errno));
                line_irq = NULL;
            }
        }
    }
    if (!line_irq) {
        fprintf(stderr, "touch_driver_init: IRQ GPIO not available, "
                "falling back to continuous SPI polling\n");
    }

    /* --- Start polling thread ------------------------------------- */
    poll_running = true;
    if (pthread_create(&poll_thread, NULL, touch_poll_thread, NULL) != 0) {
        fprintf(stderr, "touch_driver_init: failed to create poll thread\n");
        touch_driver_deinit();
        return -1;
    }

    /* --- LVGL input device registration (9.x API only) ------------ */
    indev = lv_indev_create();
    if (!indev) {
        fprintf(stderr, "touch_driver_init: lv_indev_create failed\n");
        touch_driver_deinit();
        return -1;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    fprintf(stderr, "touch_driver_init: XPT2046 ready, polling at %d Hz\n",
            TOUCH_POLL_HZ);
    return 0;
}

void touch_driver_deinit(void)
{
    /* Stop polling thread */
    if (poll_running) {
        poll_running = false;
        pthread_join(poll_thread, NULL);
    }

    /* Close SPI */
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }

    /* Release GPIO */
    if (line_irq)        { gpiod_line_release(line_irq);        line_irq = NULL; }
    if (gpio_chip_touch) { gpiod_chip_close(gpio_chip_touch);   gpio_chip_touch = NULL; }

    indev = NULL;
}
