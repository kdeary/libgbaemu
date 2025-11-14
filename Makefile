SHELL := /bin/sh

SRC_DIR := src
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
INCLUDE_DIR := include

WITH_DEBUGGER ?= 1

SDL2_CONFIG ?= sdl2-config
SDL2_CFLAGS ?= $(shell $(SDL2_CONFIG) --cflags 2>/dev/null)
SDL2_LIBS ?= $(shell $(SDL2_CONFIG) --libs 2>/dev/null)

STACK_USAGE ?= 0

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

LIB := $(BUILD_DIR)/libgbaemu.a
PORT_SRC := ports/sdl/main.c
PORT_OBJ := $(patsubst %.c,$(OBJ_DIR)/%.o,$(PORT_SRC))
PORT_BIN := $(BUILD_DIR)/gba-sdl

# ---- Profiling (separate build dir) ----
PROFILE_FLAGS = -pg
PROFILE_BUILD_DIR := $(BUILD_DIR)/profile
OBJ_DIR_PROFILE := $(PROFILE_BUILD_DIR)/obj
LIB_PROFILE := $(PROFILE_BUILD_DIR)/libgbaemu.a
PORT_OBJ_PROFILE := $(patsubst %.c,$(OBJ_DIR_PROFILE)/%.o,$(PORT_SRC))
PORT_BIN_PROFILE := $(PROFILE_BUILD_DIR)/gba-sdl
OBJ_PROFILE := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR_PROFILE)/%.o,$(SRC))
# ----------------------------------------

CC ?= cc
AR ?= ar

TEST_ARGS = roms/emerald.gba --bios roms/gba_bios.bin

CPPFLAGS += -I$(INCLUDE_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fms-extensions
ARFLAGS ?= rcs
LIBS := -lpthread -lm

STACK_USAGE_FILE ?= $(BUILD_DIR)/stack-usage-report.txt

ifeq ($(STACK_USAGE),1)
CFLAGS += -fstack-usage
endif

.PHONY: all clean distclean \
	profile-build profile-run \
	valgrind-run memcheck perf-run \
	stack-usage

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

# =========================
#   Profiling targets
# =========================

# --- gprof build (separate tree with -pg) ---
profile-build: CFLAGS += $(PROFILE_FLAGS)
profile-build: $(LIB_PROFILE) $(PORT_BIN_PROFILE)

$(LIB_PROFILE): $(OBJ_PROFILE)
	@mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

$(OBJ_DIR_PROFILE)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR_PROFILE)/ports/sdl/%.o: ports/sdl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL2_CFLAGS) -c $< -o $@

$(PORT_BIN_PROFILE): $(LIB_PROFILE) $(PORT_OBJ_PROFILE)
	@mkdir -p $(dir $@)
	# Link with -pg via target-specific CFLAGS
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) $(PORT_OBJ_PROFILE) $(LIB_PROFILE) $(SDL2_LIBS) $(LIBS) -o $@

# --- gprof run + report (robust) ---
profile-run: profile-build
	@mkdir -p $(PROFILE_BUILD_DIR)
	$(PORT_BIN_PROFILE) $(TEST_ARGS)

	# Move default gmon.out if it was written in CWD
	@if [ -f gmon.out ]; then mv gmon.out $(PROFILE_BUILD_DIR)/gmon.out; fi

	# Build report from any gmon.out variant we have
	@if ls $(PROFILE_BUILD_DIR)/gmon.out* >/dev/null 2>&1; then \
		gprof $(PORT_BIN_PROFILE) $(PROFILE_BUILD_DIR)/gmon.out* > $(PROFILE_BUILD_DIR)/gprof-report.txt; \
		echo "✅ gprof report: $(PROFILE_BUILD_DIR)/gprof-report.txt"; \
	else \
		echo "❌ No gmon.out produced. Make sure you rebuilt with -pg (use: make profile-build) and that your system supports gprof."; \
		exit 1; \
	fi

# --- Valgrind Massif (heap profiler) ---
valgrind-run: $(PORT_BIN)
	@command -v valgrind >/dev/null || { echo "❌ valgrind not found. Please install it to run this target."; exit 127; }
	@command -v ms_print >/dev/null || { echo "❌ ms_print not found. Install valgrind's tools to continue."; exit 127; }
	@mkdir -p $(BUILD_DIR)
	@out_file=$$(mktemp $(BUILD_DIR)/massif.out.XXXXXX); \
		echo "▶️  Running Massif, output -> $$out_file"; \
		if valgrind --tool=massif --suppressions=bench/valgrind_sdl.suppression --time-unit=ms --massif-out-file=$$out_file \
				--ignore-fn=SDL_* --ignore-fn=*_gallium* --ignore-fn=*_mesa* \
			$(PORT_BIN) $(TEST_ARGS); then \
			MS_PRINT_EXEC=$(PORT_BIN) ms_print $$out_file > $(BUILD_DIR)/massif-report.txt; \
			echo "✅ Massif report with symbols: $(BUILD_DIR)/massif-report.txt"; \
		else \
			echo "❌ Massif run failed. See output above."; \
			rm -f $$out_file; \
			exit 1; \
		fi

# --- Valgrind memcheck (leaks + misuse) ---
memcheck: $(PORT_BIN)
	@valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes $(PORT_BIN) $(TEST_ARGS)

# --- perf sampling (CPU hotspots) ---
perf-run: $(PORT_BIN)
	@perf record -g --call-graph dwarf $(PORT_BIN) $(TEST_ARGS)
	@perf report

# --- Stack usage aggregation ---
stack-usage: clean
	@$(MAKE) STACK_USAGE=1 all >/dev/null
	@mkdir -p $(BUILD_DIR)
	@su_files=$$(find $(BUILD_DIR) -name '*.su' | sort); \
	if [ -z "$$su_files" ]; then \
		echo "❌ No stack usage files produced. Does your compiler support -fstack-usage?"; \
		exit 1; \
	fi; \
	out="$(STACK_USAGE_FILE)"; \
	overall=0; \
	{ \
		echo "# Stack usage report"; \
		echo "# Generated: $$(date -u)"; \
		echo "# Toolchain: $(CC)"; \
	} > $$out; \
	for f in $$su_files; do \
		echo >> $$out; \
		echo "## $$f" >> $$out; \
		cat $$f >> $$out; \
		file_total=$$(awk '{ for (i = 1; i <= NF; ++i) { if ($$i ~ /^[0-9]+$$/) { sum += $$i; break; } } } END { printf("%d", sum) }' $$f); \
		overall=$$((overall + file_total)); \
		echo "File total: $$file_total bytes" >> $$out; \
	done; \
	echo >> $$out; \
	echo "## Overall" >> $$out; \
	echo "Total stack usage across files: $$overall bytes" >> $$out; \
	echo "✅ Stack usage report written to $$out"
