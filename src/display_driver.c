/**
 * display_driver.c — ILI9486 SPI display driver for LVGL 9.x
 *
 * Clean LVGL 9.x implementation from scratch. No v8 API usage.
 *
 * Hardware:
 *   SPI  : /dev/spidev0.0 at 24 MHz
 *   DC   : GPIO 25 (data/command select)
 *   RST  : GPIO 27 (hardware reset)
 *   BL   : GPIO 18 (backlight enable)
 *
 * The ILI9486 is driven in 16-bit RGB565 mode, 480×320 landscape.
 * LVGL flush callback sends pixel data over SPI with DC pin high (data mode).
 * Command bytes are sent with DC pin low (command mode).
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
#include <linux/spi/spidev.h>

/* ------------------------------------------------------------------ */
/*  Hardware pin definitions                                          */
/* ------------------------------------------------------------------ */

#define SPI_DEVICE   "/dev/spidev0.0"
#define SPI_SPEED_HZ 24000000   /* 24 MHz */
#define SPI_MODE     SPI_MODE_0
#define SPI_BPW      8          /* bits per word */

#define GPIO_DC  25   /* Data / Command select */
#define GPIO_RST 27   /* Hardware reset         */
#define GPIO_BL  18   /* Backlight enable       */

/* Partial-update draw buffer: 10 lines × 480 pixels × 2 bytes (RGB565) */
#define DRAW_BUF_LINES 10
#define DRAW_BUF_SIZE  (DISP_HOR_RES * DRAW_BUF_LINES * sizeof(lv_color16_t))

/* ------------------------------------------------------------------ */
/*  Module-level state                                                */
/* ------------------------------------------------------------------ */

static int spi_fd = -1;          /* SPI file descriptor              */
static int gpio_dc_fd = -1;      /* GPIO value fd for DC pin         */
static int gpio_rst_fd = -1;     /* GPIO value fd for RST pin        */
static int gpio_bl_fd = -1;      /* GPIO value fd for BL pin         */

static lv_display_t *disp = NULL; /* LVGL display handle              */
static uint8_t *draw_buf = NULL;  /* LVGL draw buffer                 */

/* ------------------------------------------------------------------ */
/*  GPIO helpers (sysfs interface)                                    */
/* ------------------------------------------------------------------ */

/**
 * Export a GPIO pin via /sys/class/gpio/export and set its direction.
 * Returns an open fd to the pin's value file, or -1 on error.
 */
static int gpio_export_and_open(int pin, const char *direction)
{
    char path[64];
    int fd;

    /* Export the pin (ignore EBUSY — already exported) */
    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d", pin);
        if (write(fd, buf, len) < 0 && errno != EBUSY) {
            perror("gpio export write");
        }
        close(fd);
    }

    /* Brief delay for sysfs to create the pin directory */
    usleep(100000);

    /* Set direction */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "gpio_export_and_open: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (write(fd, direction, strlen(direction)) < 0) {
        perror("gpio direction write");
        close(fd);
        return -1;
    }
    close(fd);

    /* Open the value file for fast writes */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "gpio_export_and_open: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    return fd;
}

/** Write a 0 or 1 to an already-open GPIO value fd. */
static void gpio_write(int fd, int value)
{
    const char *v = value ? "1" : "0";
    if (write(fd, v, 1) < 0) {
        perror("gpio_write");
    }
}

/** Unexport a GPIO pin and close its value fd. */
static void gpio_unexport(int pin, int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    int exp_fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (exp_fd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d", pin);
        if (write(exp_fd, buf, len) < 0) {
            /* Ignore errors — pin may not be exported */
        }
        close(exp_fd);
    }
}

/* ------------------------------------------------------------------ */
/*  SPI helpers                                                       */
/* ------------------------------------------------------------------ */

/** Open and configure the SPI device. Returns 0 on success. */
static int spi_init(void)
{
    spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        fprintf(stderr, "spi_init: cannot open %s: %s\n",
                SPI_DEVICE, strerror(errno));
        return -1;
    }

    uint8_t mode = SPI_MODE;
    uint8_t bpw  = SPI_BPW;
    uint32_t speed = SPI_SPEED_HZ;

    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("spi_init: SPI_IOC_WR_MODE");
        return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0) {
        perror("spi_init: SPI_IOC_WR_BITS_PER_WORD");
        return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("spi_init: SPI_IOC_WR_MAX_SPEED_HZ");
        return -1;
    }

    return 0;
}

/** Transfer a buffer over SPI. */
static void spi_transfer(const uint8_t *data, size_t len)
{
    struct spi_ioc_transfer tr = {
        .tx_buf        = (unsigned long)data,
        .rx_buf        = 0,
        .len           = (uint32_t)len,
        .speed_hz      = SPI_SPEED_HZ,
        .bits_per_word = SPI_BPW,
        .delay_usecs   = 0,
    };

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        perror("spi_transfer");
    }
}

/* ------------------------------------------------------------------ */
/*  ILI9486 command / data helpers                                    */
/* ------------------------------------------------------------------ */

/** Send a single command byte (DC low). */
static void ili_send_cmd(uint8_t cmd)
{
    gpio_write(gpio_dc_fd, 0);  /* command mode */
    spi_transfer(&cmd, 1);
}

/** Send data bytes (DC high). */
static void ili_send_data(const uint8_t *data, size_t len)
{
    gpio_write(gpio_dc_fd, 1);  /* data mode */
    spi_transfer(data, len);
}

/** Send a single data byte. */
static void ili_send_data_byte(uint8_t val)
{
    ili_send_data(&val, 1);
}

/* ------------------------------------------------------------------ */
/*  ILI9486 initialisation sequence                                   */
/* ------------------------------------------------------------------ */

/** Hardware reset: pull RST low for 10ms, then high, wait 120ms. */
static void ili_hw_reset(void)
{
    gpio_write(gpio_rst_fd, 1);
    usleep(10000);
    gpio_write(gpio_rst_fd, 0);
    usleep(10000);
    gpio_write(gpio_rst_fd, 1);
    usleep(120000);
}

/**
 * Run the ILI9486 initialisation command sequence.
 *
 * Configures the controller for:
 *   - 16-bit RGB565 pixel format
 *   - Landscape orientation (480×320)
 *   - Normal display mode, display ON
 */
static void ili_init_sequence(void)
{
    /* Software reset */
    ili_send_cmd(0x01);
    usleep(120000);

    /* Sleep out */
    ili_send_cmd(0x11);
    usleep(120000);

    /* Interface pixel format: 16-bit/pixel (RGB565) */
    ili_send_cmd(0x3A);
    ili_send_data_byte(0x55);

    /* Memory access control: landscape orientation
     * Bit 5 (MV) = 1 : row/column exchange
     * Bit 6 (MX) = 1 : column address order (mirror X)
     * Result: 480 wide × 320 tall landscape */
    ili_send_cmd(0x36);
    ili_send_data_byte(0x28);

    /* Power control 1 */
    ili_send_cmd(0xC0);
    ili_send_data_byte(0x0E);
    ili_send_data_byte(0x0E);

    /* Power control 2 */
    ili_send_cmd(0xC1);
    ili_send_data_byte(0x41);
    ili_send_data_byte(0x00);

    /* VCOM control */
    ili_send_cmd(0xC5);
    ili_send_data_byte(0x00);
    ili_send_data_byte(0x22);
    ili_send_data_byte(0x80);

    /* Frame rate control: 60 Hz */
    ili_send_cmd(0xB1);
    ili_send_data_byte(0xB0);
    ili_send_data_byte(0x11);

    /* Display inversion control: 2-dot inversion */
    ili_send_cmd(0xB4);
    ili_send_data_byte(0x02);

    /* Display function control */
    ili_send_cmd(0xB6);
    ili_send_data_byte(0x02);
    ili_send_data_byte(0x22);

    /* Normal display mode on */
    ili_send_cmd(0x13);
    usleep(10000);

    /* Display ON */
    ili_send_cmd(0x29);
    usleep(10000);
}

/**
 * Set the ILI9486 column and row address window for a pixel region.
 * All subsequent pixel data writes fill this window left-to-right,
 * top-to-bottom.
 */
static void ili_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t col_data[4] = {
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
        (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF),
    };
    uint8_t row_data[4] = {
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
        (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF),
    };

    /* Column address set */
    ili_send_cmd(0x2A);
    ili_send_data(col_data, 4);

    /* Row address set */
    ili_send_cmd(0x2B);
    ili_send_data(row_data, 4);

    /* Memory write — subsequent data goes to framebuffer */
    ili_send_cmd(0x2C);
}

/* ------------------------------------------------------------------ */
/*  LVGL flush callback                                               */
/* ------------------------------------------------------------------ */

/**
 * LVGL 9.x flush callback.
 *
 * Called by LVGL when a region of the draw buffer is ready to be sent
 * to the display. Sets the ILI9486 address window, sends pixel data
 * over SPI, then signals LVGL that the flush is complete.
 *
 * Requirement 1.3: call lv_display_flush_ready(disp) on completion.
 * Requirement 1.4: uses ONLY LVGL 9.x API (not lv_disp_flush_ready).
 */
static void disp_flush_cb(lv_display_t *display, const lv_area_t *area,
                           uint8_t *px_map)
{
    uint16_t x1 = (uint16_t)area->x1;
    uint16_t y1 = (uint16_t)area->y1;
    uint16_t x2 = (uint16_t)area->x2;
    uint16_t y2 = (uint16_t)area->y2;

    /* Set the target window on the ILI9486 */
    ili_set_window(x1, y1, x2, y2);

    /* Calculate total bytes: width × height × 2 bytes per pixel (RGB565) */
    uint32_t width  = (uint32_t)(x2 - x1 + 1);
    uint32_t height = (uint32_t)(y2 - y1 + 1);
    uint32_t size   = width * height * 2;

    /* Send pixel data (DC high = data mode) */
    ili_send_data(px_map, size);

    /* Signal LVGL that flush is complete — LVGL 9.x API */
    lv_display_flush_ready(display);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * Initialise the ILI9486 display and register with LVGL 9.x.
 *
 * Requirement 1.1: opens /dev/spidev0.0 at 24 MHz, GPIO DC=25, RST=27, BL=18
 * Requirement 1.2: registers via lv_display_create(480, 320) + lv_display_set_flush_cb
 * Requirement 1.3: flush callback calls lv_display_flush_ready(disp)
 * Requirement 1.4: no v8 API usage
 */
int display_driver_init(void)
{
    /* --- SPI setup ------------------------------------------------ */
    if (spi_init() != 0) {
        fprintf(stderr, "display_driver_init: SPI init failed\n");
        return -1;
    }

    /* --- GPIO setup ----------------------------------------------- */
    gpio_dc_fd  = gpio_export_and_open(GPIO_DC,  "out");
    gpio_rst_fd = gpio_export_and_open(GPIO_RST, "out");
    gpio_bl_fd  = gpio_export_and_open(GPIO_BL,  "out");

    if (gpio_dc_fd < 0 || gpio_rst_fd < 0 || gpio_bl_fd < 0) {
        fprintf(stderr, "display_driver_init: GPIO init failed\n");
        display_driver_deinit();
        return -1;
    }

    /* --- ILI9486 hardware init ------------------------------------ */
    ili_hw_reset();
    ili_init_sequence();

    /* Turn on backlight */
    gpio_write(gpio_bl_fd, 1);

    /* --- LVGL display registration (9.x API only) ----------------- */

    /* Allocate draw buffer */
    draw_buf = (uint8_t *)malloc(DRAW_BUF_SIZE);
    if (!draw_buf) {
        fprintf(stderr, "display_driver_init: draw buffer alloc failed\n");
        display_driver_deinit();
        return -1;
    }

    /* Create display — LVGL 9.x API (not lv_disp_drv_init/register) */
    disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    if (!disp) {
        fprintf(stderr, "display_driver_init: lv_display_create failed\n");
        display_driver_deinit();
        return -1;
    }

    /* Attach draw buffer — LVGL 9.x API (not lv_disp_draw_buf_init) */
    lv_display_set_buffers(disp, draw_buf, NULL, DRAW_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Set flush callback — LVGL 9.x API */
    lv_display_set_flush_cb(disp, disp_flush_cb);

    fprintf(stderr, "display_driver_init: ILI9486 480x320 ready\n");
    return 0;
}

void display_driver_deinit(void)
{
    /* Turn off backlight */
    if (gpio_bl_fd >= 0) {
        gpio_write(gpio_bl_fd, 0);
    }

    /* Close SPI */
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }

    /* Unexport GPIOs */
    gpio_unexport(GPIO_DC,  &gpio_dc_fd);
    gpio_unexport(GPIO_RST, &gpio_rst_fd);
    gpio_unexport(GPIO_BL,  &gpio_bl_fd);

    /* Free draw buffer */
    if (draw_buf) {
        free(draw_buf);
        draw_buf = NULL;
    }

    /* LVGL display handle is managed by LVGL — don't free manually */
    disp = NULL;
}
