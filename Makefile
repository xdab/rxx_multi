# Compiler and flags
CC := gcc
CFLAGS := -O3 -march=native -mtune=native -Wall -Wextra -I./include
LIBS_COMMON := -lpthread -lm -lliquid

# Backends: rtl (librtlsdr) and rsp (SDRplay API v3)
BACKENDS ?= rtl rsp

# Profiling build (make profile): production codegen + debug symbols for
# valgrind/callgrind, in a separate obj dir and *.prof binaries
ifeq ($(PROFILE),1)
CFLAGS += -g -fno-omit-frame-pointer
OBJ_DIR := obj/prof
TARGET_SUFFIX := .prof
endif

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
rtl_TARGET := $(BIN_DIR)/rtl_multi$(TARGET_SUFFIX)
rsp_TARGET := $(BIN_DIR)/rsp_multi$(TARGET_SUFFIX)

# `all` must stay the first non-special rule (the default goal): the Windows
# block below defines `dlls`, which would otherwise silently become what bare
# `make` runs. ALL_EXTRA is resolved here, before the block appends to it.
WIN_FLAVOR := $(findstring Windows_NT,$(OS))$(findstring _NT,$(shell uname -s 2>/dev/null))
ALL_EXTRA :=
ifneq ($(WIN_FLAVOR),)
ALL_EXTRA := dlls
endif
.PHONY: all
all: $(BACKENDS) $(ALL_EXTRA)

# Windows (UCRT64): the SDRplay API is a system install, not a pacman package
ifneq ($(WIN_FLAVOR),)
# pin make's own sh: a foreign MSYS flavor in PATH (e.g. Git Bash) mounts /ucrt64 elsewhere
SHELL := /bin/sh
# make strips TMP/TEMP from recipe environments; mingw gcc resolves its
# scratch dir via GetTempPath() (TMP, TEMP, USERPROFILE) and dies with
# "Cannot create temporary file in C:\windows\" without them
export TMP := $(TMP)
export TEMP := $(TEMP)
LIBS_COMMON += -lws2_32
SDRPLAY_HOME ?= "C:/Program Files/SDRplay/API"
CFLAGS += -I$(SDRPLAY_HOME)/inc
rsp_LIBS += -L$(SDRPLAY_HOME)/x64

# stage every import-table DLL that exists in the toolchain bin beside the
# binaries (DLL search starts there); two passes close the chain one level
# down. System DLLs and the non-redistributable sdrplay_api.dll drop out on
# the existence check.
GCC_ROOT := $(shell LC_ALL=C $(CC) -print-search-dirs 2>/dev/null | tr -d '\r' | sed -n '/^install: /{s|^install: ||;s|/lib/gcc/.*||;s|/bin/\.\.||;p}')
UCRT64_BIN ?= $(GCC_ROOT)/bin
DLL_IMPORTS := sed -n 's/[[:space:]]*DLL Name: //p'
STAGE_DLLS = while read dll; do [ -f $(UCRT64_BIN)/$$dll ] && echo $(UCRT64_BIN)/$$dll; done | sort -u | xargs -r cp -t $(BIN_DIR)/

.PHONY: dlls
dlls: | $(BIN_DIR)
	@rm -f $(BIN_DIR)/*.dll
	@for f in $(BIN_DIR)/*.exe; do [ -f $$f ] && objdump -p $$f; done | $(DLL_IMPORTS) | sort -u | $(STAGE_DLLS)
	@for f in $(BIN_DIR)/*.dll; do [ -f $$f ] && objdump -p $$f; done | $(DLL_IMPORTS) | sort -u | $(STAGE_DLLS)
	@echo "✓ runtime DLLs staged in $(BIN_DIR)/"
endif

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

$(BIN_DIR)/$(2)$(TARGET_SUFFIX): $$($(1)_OBJ) | $(BIN_DIR)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $(LIBS_COMMON) $$($(1)_LIBS)
	@echo "✓ Build successful: $$@"
endef

$(eval $(call BACKEND_RULES,rtl,rtl_multi))
$(eval $(call BACKEND_RULES,rsp,rsp_multi))

.PHONY: rtl rsp
rtl: $(rtl_TARGET)
rsp: $(rsp_TARGET)

# Opt-in Windows GUI launcher (not part of `all`); plain Win32, links
# against the default MinGW import libs, no new dependencies
ifneq ($(WIN_FLAVOR),)
.PHONY: gui
gui: src/gui/main.c src/gui/shell.c src/gui/args_mapper.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -mwindows src/gui/main.c src/gui/shell.c src/gui/args_mapper.c -o $(BIN_DIR)/rxx_gui.exe -lcomdlg32 -lcomctl32
	@echo "✓ Build successful: $(BIN_DIR)/rxx_gui.exe"
endif

# GUI arg-mapper unit tests (pure C, no Win32, runs anywhere)
.PHONY: gui-test
gui-test: src/gui/args_mapper.c src/gui/args_mapper_test.c | $(BIN_DIR)
	$(CC) $(CFLAGS) src/gui/args_mapper.c src/gui/args_mapper_test.c -o $(BIN_DIR)/rxx_gui_test$(TARGET_SUFFIX)
	$(BIN_DIR)/rxx_gui_test$(TARGET_SUFFIX)

# Profiling builds (valgrind/callgrind); do not touch obj/ or bin/ builds
.PHONY: profile profile-rtl profile-rsp
profile: profile-rtl profile-rsp
profile-rtl:
	$(MAKE) PROFILE=1 rtl
profile-rsp:
	$(MAKE) PROFILE=1 rsp

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

# API docs (Doxyfile at repo root; output in docs/doxygen/html)
.PHONY: doc
doc:
	@command -v doxygen >/dev/null 2>&1 || { \
		echo "ERROR: doxygen not installed (Arch: sudo pacman -S doxygen)"; exit 1; }
	doxygen Doxyfile
	@echo "✓ API docs: docs/doxygen/html/index.html"

# Style: C via clang-format (src/, include/), tools/*.py via ruff (config in
# pyproject.toml). fmt-check is the gate counterpart — it exits nonzero on any
# finding, so it can sit in CI or a pre-commit hook.
C_SOURCES := $(wildcard src/core/*.c) $(wildcard src/backend/*/*.c) $(wildcard src/gui/*.c) $(wildcard include/*.h)
PY_SOURCES := $(wildcard tools/*.py)

.PHONY: fmt
fmt:
	@command -v clang-format >/dev/null 2>&1 || { \
		echo "ERROR: clang-format not installed (Arch: sudo pacman -S clang)"; exit 1; }
	@command -v ruff >/dev/null 2>&1 || { \
		echo "ERROR: ruff not installed (Arch: sudo pacman -S ruff)"; exit 1; }
	clang-format -i $(C_SOURCES)
	ruff format $(PY_SOURCES)
	ruff check --fix $(PY_SOURCES)

.PHONY: fmt-check
fmt-check:
	@command -v clang-format >/dev/null 2>&1 || { \
		echo "ERROR: clang-format not installed (Arch: sudo pacman -S clang)"; exit 1; }
	@command -v ruff >/dev/null 2>&1 || { \
		echo "ERROR: ruff not installed (Arch: sudo pacman -S ruff)"; exit 1; }
	clang-format --dry-run --Werror $(C_SOURCES)
	ruff format --check $(PY_SOURCES)
	ruff check $(PY_SOURCES)

# Compile database for editor IntelliSense and clang-tidy (make compdb).
# Entries mirror the per-backend build flags; `tidy` analyzes core + RTL only —
# RSP needs the SDRplay API header, which CI doesn't install (same scope
# decision as the build job).
COMPDB := compile_commands.json
TIDY_SRCS := $(CORE_SRC) $(wildcard src/backend/rtl/*.c)

# native Windows clang tools need a Windows-form directory (msys CURDIR is not)
WIN_CURDIR := $(if $(WIN_FLAVOR),$(shell cygpath -m $(CURDIR)),$(CURDIR))

.PHONY: compdb
compdb:
	@: > $(COMPDB); echo '[' >> $(COMPDB)
	@q='\"'; \
	for src in $(TIDY_SRCS); do \
		{ printf '{"directory": "%s", "file": "%s", "arguments": ["%s"' "$(WIN_CURDIR)" "$$src" "$(CC)"; \
		  for fl in $(CFLAGS); do printf ', "%s"' "$$fl"; done; \
		  printf ', "-DAPP_NAME=%srtl_multi%s"' "$$q" "$$q"; \
		  printf ', "-c", "%s"]},\n' "$$src"; } >> $(COMPDB); \
	done
	@for src in $(wildcard src/backend/rsp/*.c); do \
		{ printf '{"directory": "%s", "file": "%s", "arguments": ["%s"' "$(WIN_CURDIR)" "$$src" "$(CC)"; \
		  for fl in $(CFLAGS); do printf ', "%s"' "$$fl"; done; \
		  printf ', "-DAPP_NAME=%srsp_multi%s"' "$$q" "$$q"; \
		  printf ', "-c", "%s"]},\n' "$$src"; } >> $(COMPDB); \
	done
	@sed -i '$$ s/},$$/}/' $(COMPDB)
	@echo ']' >> $(COMPDB)
	@echo "✓ $(COMPDB)"

# Static analysis gate (clang-tidy; check set in .clang-tidy at repo root).
# Findings exit nonzero, like fmt-check. The compdb prerequisite keeps the
# compile database fresh; clang-tidy only parses (needs headers, not libs).
.PHONY: tidy
tidy: compdb
	@command -v clang-tidy >/dev/null 2>&1 || { \
		echo "ERROR: clang-tidy not installed (Arch: sudo pacman -S clang)"; exit 1; }
	clang-tidy -p . $(TIDY_SRCS) --warnings-as-errors=*

# Help target
.PHONY: help
help:
	@echo "rxx_multi Makefile Targets"
	@echo "=========================="
	@echo "  all         - Build all backends (default: BACKENDS=rtl rsp)"
	@echo "  rtl         - Build bin/rtl_multi (librtlsdr)"
	@echo "  rsp         - Build bin/rsp_multi (SDRplay API v3)"
	@echo "  gui         - Build the opt-in Windows GUI launcher (bin/rxx_gui.exe)"
	@echo "  gui-test    - Build and run the GUI arg-mapper unit tests"
	@echo "  dlls        - Stage runtime DLLs in bin/ (Windows only)"
	@echo "  profile     - Build bin/*.prof with debug symbols (valgrind/callgrind)"
	@echo "  clean       - Remove build artifacts (obj/, bin/)"
	@echo "  doc         - Generate API docs with doxygen"
	@echo "  fmt         - Format src/, include/ (clang-format) and tools/*.py (ruff)"
	@echo "  fmt-check   - Style gate: nonzero on any formatting/lint finding"
	@echo "  compdb      - Regenerate compile_commands.json (editor IntelliSense)"
	@echo "  tidy        - clang-tidy over core + RTL backend (gate, nonzero on findings)"
	@echo "  install     - Install both binaries to /usr/local/bin"
	@echo ""
	@echo "Usage:"
	@echo "  make                # Build both binaries"
	@echo "  make rtl            # Build only rtl_multi"
	@echo "  make rsp            # Build only rsp_multi"
	@echo "  make clean          # Clean build artifacts"
