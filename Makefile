# Compiler and flags
CC := gcc
CFLAGS := -O3 -march=native -mtune=native -Wall -Wextra -I./include
LIBS_COMMON := -lpthread -lm -lliquid

# Backends: rtl (librtlsdr) and rsp (SDRplay API v3)
BACKENDS ?= rtl rsp

# Directories
CORE_DIR := src/core
OBJ_DIR := obj
BIN_DIR := bin

# Core sources (shared by all backends)
CORE_SRC := $(wildcard $(CORE_DIR)/*.c)

# Per-backend sources and link flags
rtl_LIBS := -lrtlsdr
rsp_LIBS := -lsdrplay_api

# Binary names carry the backend name; APP_NAME drives the help text
rtl_TARGET := $(BIN_DIR)/rtl_multi
rsp_TARGET := $(BIN_DIR)/rsp_multi

.PHONY: all
all: $(BACKENDS)

# Create necessary directories
$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Compile core sources once per backend (APP_NAME differs)
$(OBJ_DIR)/rtl/core/%.o: $(CORE_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DAPP_NAME='"rtl_multi"' -MMD -MP -c $< -o $@

$(OBJ_DIR)/rsp/core/%.o: $(CORE_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DAPP_NAME='"rsp_multi"' -MMD -MP -c $< -o $@

# Compile backend sources
define BACKEND_RULES
$(1)_OBJ := $(patsubst $(CORE_DIR)/%.c,$(OBJ_DIR)/$(1)/core/%.o,$(CORE_SRC)) \
            $(patsubst src/backend/$(1)/%.c,$(OBJ_DIR)/$(1)/backend/%.o,$(wildcard src/backend/$(1)/*.c))

$(OBJ_DIR)/$(1)/backend/%.o: src/backend/$(1)/%.c | $(OBJ_DIR)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -DAPP_NAME='"$(2)"' -MMD -MP -c $$< -o $$@

$(BIN_DIR)/$(2): $$($(1)_OBJ) | $(BIN_DIR)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $(LIBS_COMMON) $$($(1)_LIBS)
	@echo "✓ Build successful: $$@"
endef

$(eval $(call BACKEND_RULES,rtl,rtl_multi))
$(eval $(call BACKEND_RULES,rsp,rsp_multi))

.PHONY: rtl rsp
rtl: $(rtl_TARGET)
rsp: $(rsp_TARGET)

-include $(shell find $(OBJ_DIR) -name '*.d' 2>/dev/null)

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "✓ Clean complete"

.PHONY: install
install: all
	install -D $(rtl_TARGET) /usr/local/bin/rtl_multi
	install -D $(rsp_TARGET) /usr/local/bin/rsp_multi
	@echo "✓ Installed to /usr/local/bin"

.PHONY: uninstall
uninstall:
	rm -f /usr/local/bin/rtl_multi /usr/local/bin/rsp_multi
	@echo "✓ Uninstalled"

# Help target
.PHONY: help
help:
	@echo "rxx_multi Makefile Targets"
	@echo "=========================="
	@echo "  all         - Build all backends (default: BACKENDS=rtl rsp)"
	@echo "  rtl         - Build bin/rtl_multi (librtlsdr)"
	@echo "  rsp         - Build bin/rsp_multi (SDRplay API v3)"
	@echo "  clean       - Remove build artifacts (obj/, bin/)"
	@echo "  install     - Install both binaries to /usr/local/bin"
	@echo ""
	@echo "Usage:"
	@echo "  make                # Build both binaries"
	@echo "  make rtl            # Build only rtl_multi"
	@echo "  make rsp            # Build only rsp_multi"
	@echo "  make clean          # Clean build artifacts"
