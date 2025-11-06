SHELL := /bin/sh

SRC_DIR := src
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
INCLUDE_DIR := include

WITH_DEBUGGER ?= 1

SDL2_CONFIG ?= sdl2-config
SDL2_CFLAGS ?= $(shell $(SDL2_CONFIG) --cflags 2>/dev/null)
SDL2_LIBS ?= $(shell $(SDL2_CONFIG) --libs 2>/dev/null)

SRC := \
	$(SRC_DIR)/apu/apu.c \
	$(SRC_DIR)/apu/fifo.c \
	$(SRC_DIR)/apu/modules.c \
	$(SRC_DIR)/apu/noise.c \
	$(SRC_DIR)/apu/tone.c \
	$(SRC_DIR)/apu/wave.c \
	$(SRC_DIR)/channel.c \
	$(SRC_DIR)/core/arm/alu.c \
	$(SRC_DIR)/core/arm/bdt.c \
	$(SRC_DIR)/core/arm/branch.c \
	$(SRC_DIR)/core/arm/core.c \
	$(SRC_DIR)/core/arm/mul.c \
	$(SRC_DIR)/core/arm/psr.c \
	$(SRC_DIR)/core/arm/sdt.c \
	$(SRC_DIR)/core/arm/swi.c \
	$(SRC_DIR)/core/arm/swp.c \
	$(SRC_DIR)/core/core.c \
	$(SRC_DIR)/core/thumb/alu.c \
	$(SRC_DIR)/core/thumb/bdt.c \
	$(SRC_DIR)/core/thumb/branch.c \
	$(SRC_DIR)/core/thumb/core.c \
	$(SRC_DIR)/core/thumb/logical.c \
	$(SRC_DIR)/core/thumb/sdt.c \
	$(SRC_DIR)/core/thumb/swi.c \
	$(SRC_DIR)/db.c \
	$(SRC_DIR)/debugger.c \
	$(SRC_DIR)/gba.c \
	$(SRC_DIR)/gpio/gpio.c \
	$(SRC_DIR)/gpio/rtc.c \
	$(SRC_DIR)/gpio/rumble.c \
	$(SRC_DIR)/memory/dma.c \
	$(SRC_DIR)/memory/io.c \
	$(SRC_DIR)/memory/memory.c \
	$(SRC_DIR)/memory/storage/eeprom.c \
	$(SRC_DIR)/memory/storage/flash.c \
	$(SRC_DIR)/memory/storage/storage.c \
	$(SRC_DIR)/ppu/background/affine.c \
	$(SRC_DIR)/ppu/background/bitmap.c \
	$(SRC_DIR)/ppu/background/text.c \
	$(SRC_DIR)/ppu/oam.c \
	$(SRC_DIR)/ppu/ppu.c \
	$(SRC_DIR)/ppu/window.c \
	$(SRC_DIR)/quicksave.c \
	$(SRC_DIR)/scheduler.c \
	$(SRC_DIR)/timer.c

ifeq ($(WITH_DEBUGGER),0)
SRC := $(filter-out $(SRC_DIR)/debugger.c,$(SRC))
else
CPPFLAGS += -DWITH_DEBUGGER
endif

OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC))

LIB := $(BUILD_DIR)/libgba.a
PORT_SRC := ports/sdl/main.c
PORT_OBJ := $(patsubst %.c,$(OBJ_DIR)/%.o,$(PORT_SRC))
PORT_BIN := $(BUILD_DIR)/gba-sdl

CC ?= cc
AR ?= ar

CPPFLAGS += -I$(INCLUDE_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fms-extensions
ARFLAGS ?= rcs
LIBS := -lpthread -lm

.PHONY: all clean distclean

all: $(LIB) $(PORT_BIN)

$(LIB): $(OBJ)
	@mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/ports/sdl/%.o: ports/sdl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL2_CFLAGS) -c $< -o $@

$(PORT_BIN): $(LIB) $(PORT_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) $(PORT_OBJ) $(LIB) $(SDL2_LIBS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
