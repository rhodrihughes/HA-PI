# Home Assistant Light Control — Makefile
# Cross-compile: make CC=arm-linux-gnueabihf-gcc

CC       ?= gcc
CFLAGS   := -Wall -Wextra -O2 -Iinclude -Ilvgl -I.
LDFLAGS  := -lcurl -lcrypt -lpthread -lm

# LVGL sources (add as they are created)
LVGL_SRC := $(wildcard lvgl/src/**/*.c) $(wildcard lvgl/src/*.c)

# App sources
APP_SRC  := $(wildcard src/*.c)

# Mongoose (single-file library, lives in src/)
MONGOOSE_SRC := $(wildcard src/mongoose.c)

SRC      := $(APP_SRC) $(LVGL_SRC)
OBJ      := $(SRC:.c=.o)
TARGET   := ha_lights

PI_HOST  ?= pi@raspberrypi.local
PI_DEST  ?= /home/pi/ha-pi

.PHONY: all clean deploy

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

deploy: $(TARGET)
	scp $(TARGET) $(PI_HOST):$(PI_DEST)/
	scp ha-pi.service $(PI_HOST):/tmp/ha-pi.service
	ssh $(PI_HOST) "sudo cp /tmp/ha-pi.service /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable ha-pi && sudo systemctl restart ha-pi"
