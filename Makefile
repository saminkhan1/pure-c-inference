# voxtral.c — Voxtral Realtime 4B Pure C Inference Engine
# Makefile

VOXTRAL_VERSION = 0.1.0-beta

CC = gcc
CFLAGS_BASE = -Wall -Wextra -O3 -march=native -ffast-math
LDFLAGS = -lm

# Platform detection
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Pre-build dependency check
check-deps:
	@if ! command -v $(CC) >/dev/null 2>&1; then \
		echo "Error: $(CC) not found. Install Xcode Command Line Tools:"; \
		echo "  xcode-select --install"; \
		exit 1; \
	fi
	@echo "Dependencies OK"

# Source files
SRCS = voxtral.c voxtral_kernels.c voxtral_audio.c voxtral_encoder.c voxtral_decoder.c voxtral_tokenizer.c voxtral_safetensors.c
OBJS = $(SRCS:.c=.o)
MAIN = main.c
TARGET = voxtral

# Debug build flags
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -DDEBUG -fsanitize=address

.PHONY: all clean debug info help blas mps wexproflow inspect test install install-beta uninstall app dmg

# Default: show available targets
all: help

help:
	@echo "voxtral.c — Voxtral Realtime 4B - Build Targets"
	@echo ""
	@echo "Choose a backend:"
	@echo "  make blas     - With BLAS acceleration (Accelerate/OpenBLAS)"
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
	@echo "  make mps      - Apple Silicon with Metal GPU (fastest)"
	@echo "  make wexproflow - MPS + hotkey dictation mode (Command+R → paste)"
endif
endif
	@echo ""
	@echo "Other targets:"
	@echo "  make test     - Run regression tests (slow, needs fast GPU)"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make inspect  - Build safetensors weight inspector"
	@echo "  make info     - Show build configuration"
	@echo "  make install MODEL_DIR=/path  - Install + register launchd agent (auto-start on login)"
	@echo "  make uninstall               - Unregister launchd agent and remove binary"
	@echo "  make app            - Create Voxtral.app bundle (requires prior wexproflow build)"
	@echo "  make install-beta MODEL_DIR=/path - Install app + launchd (no Apple ID needed)"
	@echo "  make dmg            - Create Voxtral-<version>.dmg for sharing (requires app)"
	@echo ""
	@echo "Example: make blas && ./voxtral -d voxtral-model -i audio.wav"

# =============================================================================
# Backend: blas (Accelerate on macOS, OpenBLAS on Linux)
# =============================================================================
ifeq ($(UNAME_S),Darwin)
SRCS += voxtral_mic_macos.c
blas: CFLAGS = $(CFLAGS_BASE) -DUSE_BLAS -DACCELERATE_NEW_LAPACK
blas: LDFLAGS += -framework Accelerate -framework AudioToolbox -framework CoreFoundation
else
blas: CFLAGS = $(CFLAGS_BASE) -DUSE_BLAS -DUSE_OPENBLAS -I/usr/include/openblas
blas: LDFLAGS += -lopenblas
SRCS += voxtral_mic_macos.c
endif
blas: check-deps clean $(TARGET)
	@echo ""
	@echo "Built with BLAS backend"

# =============================================================================
# Backend: mps (Apple Silicon Metal GPU)
# =============================================================================
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
MPS_CFLAGS = $(CFLAGS_BASE) -DUSE_BLAS -DUSE_METAL -DACCELERATE_NEW_LAPACK
MPS_OBJCFLAGS = $(MPS_CFLAGS) -fobjc-arc
MPS_LDFLAGS = $(LDFLAGS) -framework Accelerate -framework Metal -framework MetalPerformanceShaders -framework MetalPerformanceShadersGraph -framework Foundation -framework AudioToolbox -framework CoreFoundation

mps: check-deps clean mps-build
	@echo ""
	@echo "Built with MPS backend (Metal GPU acceleration)"

mps-build: $(SRCS:.c=.mps.o) voxtral_metal.o main.mps.o
	$(CC) $(MPS_CFLAGS) -o $(TARGET) $^ $(MPS_LDFLAGS)

%.mps.o: %.c voxtral.h voxtral_kernels.h
	$(CC) $(MPS_CFLAGS) -c -o $@ $<

# Embed Metal shader source as C array (runtime compilation, no Metal toolchain needed)
voxtral_shaders_source.h: voxtral_shaders.metal
	xxd -i $< > $@

voxtral_metal.o: voxtral_metal.m voxtral_metal.h voxtral_shaders_source.h
	$(CC) $(MPS_OBJCFLAGS) -c -o $@ $<

# =============================================================================
# Target: wexproflow (MPS + hotkey dictation mode)
# =============================================================================
WEXPROFLOW_SRCS = voxtral_hotkey_macos.c voxtral_paste_macos.c
WEXPROFLOW_CFLAGS = $(MPS_CFLAGS) -DWEXPROFLOW
WEXPROFLOW_OBJCFLAGS = $(WEXPROFLOW_CFLAGS) -fobjc-arc
WEXPROFLOW_LDFLAGS = -framework CoreGraphics -framework ApplicationServices \
                     -framework AppKit -framework Foundation

wexproflow: check-deps clean wexproflow-build app
	@echo ""
	@echo "Built with MPS backend + dictation mode (Command+R)"
	@echo "Created Voxtral.app for bundled dictation launch"

main.wexproflow.o: main.c voxtral.h voxtral_kernels.h voxtral_mic.h voxtral_hotkey.h voxtral_paste.h voxtral_menubar.h voxtral_sound.h
	$(CC) $(WEXPROFLOW_CFLAGS) -c -o $@ $<

voxtral_menubar.wexproflow.o: voxtral_menubar.m voxtral_menubar.h
	$(CC) $(WEXPROFLOW_OBJCFLAGS) -c -o $@ $<

wexproflow-build: $(SRCS:.c=.mps.o) $(WEXPROFLOW_SRCS:.c=.mps.o) voxtral_metal.o voxtral_menubar.wexproflow.o main.wexproflow.o
	$(CC) $(WEXPROFLOW_CFLAGS) -o $(TARGET) $^ $(MPS_LDFLAGS) $(WEXPROFLOW_LDFLAGS)

else
mps wexproflow:
	@echo "Error: MPS/wexproflow requires Apple Silicon (arm64)"
	@exit 1
endif
else
mps wexproflow:
	@echo "Error: MPS/wexproflow requires macOS"
	@exit 1
endif

# =============================================================================
# Build rules
# =============================================================================
$(TARGET): $(OBJS) main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c voxtral.h voxtral_kernels.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS += -fsanitize=address
debug: clean $(TARGET)

# =============================================================================
# Weight inspector utility
# =============================================================================
inspect: CFLAGS = $(CFLAGS_BASE)
inspect: inspect_weights.o voxtral_safetensors.o
	$(CC) $(CFLAGS) -o inspect_weights $^ $(LDFLAGS)

# =============================================================================
# Install / Uninstall (dictation service, macOS only)
# Usage: make install MODEL_DIR=/path/to/voxtral-model
# =============================================================================
INSTALL_BIN  = /usr/local/bin/voxtral
PLIST_NAME   = com.voxtral.agent
PLIST_DEST   = $(HOME)/Library/LaunchAgents/$(PLIST_NAME).plist

install: wexproflow
	@[ -n "$(MODEL_DIR)" ] || (echo "Error: specify MODEL_DIR, e.g. make install MODEL_DIR=/path/to/voxtral-model" && exit 1)
	@plutil -lint $(PLIST_NAME).plist || (echo "Error: plist validation failed" && exit 1)
	install -m 555 $(TARGET) $(INSTALL_BIN)
	-launchctl bootout gui/$$(id -u)/$(PLIST_NAME) 2>/dev/null || true
	-rm -rf $(APP_INSTALL)
	cp -r $(APP_BUNDLE) /Applications/
	xattr -rd com.apple.quarantine $(APP_INSTALL) 2>/dev/null || true
	sed -e "s|__VOXTRAL_BIN__|$(APP_INSTALL)/Contents/MacOS/$(TARGET)|g" \
	    -e "s|__MODEL_DIR__|$(MODEL_DIR)|g" \
	    -e "s|__HOME__|$(HOME)|g" \
	    $(PLIST_NAME).plist > $(PLIST_DEST)
	launchctl bootstrap gui/$$(id -u) $(PLIST_DEST)
	@echo "Installed. Voxtral.app will start on login and run in the menu bar."

uninstall:
	-launchctl bootout gui/$$(id -u)/$(PLIST_NAME) 2>/dev/null || true
	-rm -f $(PLIST_DEST)
	-rm -f $(INSTALL_BIN)
	-rm -rf $(APP_INSTALL)
	@echo "Uninstalled."

# =============================================================================
# Test
# =============================================================================
test:
	@./runtest.sh

# Fast mic start/stop cycle test — no model needed, runs in <5s
ifeq ($(UNAME_S),Darwin)
test-mic: test_mic_cycle.mps.o voxtral_mic_macos.mps.o
	$(CC) $(MPS_CFLAGS) -o test_mic_cycle $^ $(MPS_LDFLAGS)
	@echo "--- running mic cycle test ---"
	@./test_mic_cycle
	@echo "--- mic cycle test complete ---"
else
test-mic:
	@echo "test-mic: mic test only supported on macOS"
endif

# =============================================================================
# App bundle + distribution (no paid Apple Developer account required)
#
# Gatekeeper strategy for free accounts:
#   - Ad-hoc signing (codesign --sign -) is enough to satisfy macOS entitlements
#     for Metal JIT and microphone access.
#   - The quarantine xattr is what Gatekeeper actually checks. Removing it with
#     xattr -rd com.apple.quarantine lets the app run without any warning.
#   - make install-beta does both automatically — no Apple ID needed.
#   - curl downloads (not browser downloads) never get the quarantine flag,
#     so terminal-based installs work out of the box.
# =============================================================================
APP_NAME    = Voxtral
APP_BUNDLE  = $(APP_NAME).app
APP_INSTALL = /Applications/$(APP_BUNDLE)
DMG_NAME    = $(APP_NAME)-$(VOXTRAL_VERSION).dmg
DMG_STAGING = _dmg_staging

# Build the .app bundle and ad-hoc sign it.
# Ad-hoc signing (-) uses a local certificate — no Apple Developer ID needed.
# It is required so the entitlements (Metal JIT, microphone) take effect.
app:
	@[ -f $(TARGET) ] || (echo "Error: run 'make wexproflow' first" && exit 1)
	@rm -rf $(APP_BUNDLE)
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS
	@mkdir -p $(APP_BUNDLE)/Contents/Resources
	cp $(TARGET) $(APP_BUNDLE)/Contents/MacOS/$(TARGET)
	sed -e "s|__VOXTRAL_VERSION__|$(VOXTRAL_VERSION)|g" \
	    Info.plist > $(APP_BUNDLE)/Contents/Info.plist
	codesign --sign - --force --options runtime \
	    --entitlements voxtral.entitlements $(APP_BUNDLE)
	@echo "Created and ad-hoc signed $(APP_BUNDLE)"

# Install to /Applications + register launchd agent.
# No Apple Developer ID required.
# Usage: make install-beta MODEL_DIR=/path/to/voxtral-model
install-beta: wexproflow
	@[ -n "$(MODEL_DIR)" ] || (echo "Error: specify MODEL_DIR, e.g. make install-beta MODEL_DIR=$$HOME/voxtral-model" && exit 1)
	@plutil -lint $(PLIST_NAME).plist || (echo "Error: plist validation failed" && exit 1)
	# Remove existing installation gracefully
	-launchctl bootout gui/$$(id -u)/$(PLIST_NAME) 2>/dev/null || true
	-rm -rf $(APP_INSTALL)
	cp -r $(APP_BUNDLE) /Applications/
	# Strip the quarantine flag — this is what Gatekeeper checks.
	# Safe to do: we built this binary ourselves on this machine.
	xattr -rd com.apple.quarantine $(APP_INSTALL) 2>/dev/null || true
	# Register launchd agent (starts on login, restarts on crash)
	mkdir -p $(HOME)/.config/voxtral
	sed -e "s|__VOXTRAL_BIN__|$(APP_INSTALL)/Contents/MacOS/$(TARGET)|g" \
	    -e "s|__MODEL_DIR__|$(MODEL_DIR)|g" \
	    -e "s|__HOME__|$(HOME)|g" \
	    $(PLIST_NAME).plist > $(PLIST_DEST)
	launchctl bootstrap gui/$$(id -u) $(PLIST_DEST)
	@echo ""
	@echo "Installed Voxtral $(VOXTRAL_VERSION) to /Applications"
	@echo "Starts automatically on login. Running now."
	@echo "Logs: $(HOME)/.config/voxtral/voxtral.log"

# Create a DMG for sharing with beta users.
# Include a README so users know how to install without a paid Developer ID.
# Requires: brew install create-dmg
dmg: app
	@which create-dmg >/dev/null 2>&1 || \
	    (echo "Error: 'create-dmg' not found. Install with: brew install create-dmg" && exit 1)
	@rm -rf $(DMG_STAGING) $(DMG_NAME)
	@mkdir -p "$(DMG_STAGING)"
	cp -r $(APP_BUNDLE) "$(DMG_STAGING)/"
	@printf 'Voxtral %s — Install Instructions\n\n1. Drag Voxtral.app to your Applications folder.\n\n2. Open Terminal and run:\n   xattr -rd com.apple.quarantine /Applications/Voxtral.app\n\n3. Set your model path in the launchd agent:\n   make install-beta MODEL_DIR=$$HOME/voxtral-model\n\nAll transcription is on-device. Nothing leaves your Mac.\n' \
	    "$(VOXTRAL_VERSION)" > "$(DMG_STAGING)/Install Instructions.txt"
	create-dmg \
	    --volname "$(APP_NAME) $(VOXTRAL_VERSION)" \
	    --window-size 620 420 \
	    --icon-size 128 \
	    --icon "$(APP_BUNDLE)" 150 220 \
	    --app-drop-link 460 220 \
	    --hide-extension "$(APP_BUNDLE)" \
	    $(DMG_NAME) \
	    "$(DMG_STAGING)/"
	@rm -rf $(DMG_STAGING)
	@echo ""
	@echo "Created $(DMG_NAME)"
	@echo ""
	@echo "Beta install instructions for users (include in release notes):"
	@echo "  1. Download $(DMG_NAME)"
	@echo "  2. Drag Voxtral.app to /Applications"
	@echo "  3. In Terminal: xattr -rd com.apple.quarantine /Applications/Voxtral.app"
	@echo "  4. make install-beta MODEL_DIR=/path/to/voxtral-model"

# =============================================================================
# Utilities
# =============================================================================
clean:
	rm -f $(OBJS) *.mps.o *.wexproflow.o voxtral_metal.o main.o inspect_weights.o $(TARGET) inspect_weights
	rm -f voxtral_shaders_source.h voxtral_menubar.wexproflow.o
	rm -rf $(APP_BUNDLE) _dmg_staging $(APP_NAME)-*.dmg

info:
	@echo "Platform: $(UNAME_S) $(UNAME_M)"
	@echo "Compiler: $(CC)"
	@echo ""
	@echo "Available backends for this platform:"
ifeq ($(UNAME_S),Darwin)
	@echo "  blas    - Apple Accelerate"
ifeq ($(UNAME_M),arm64)
	@echo "  mps     - Metal GPU (recommended)"
endif
else
	@echo "  blas    - OpenBLAS (requires libopenblas-dev)"
endif

# =============================================================================
# Dependencies
# =============================================================================
voxtral.o: voxtral.c voxtral.h voxtral_kernels.h voxtral_safetensors.h voxtral_audio.h voxtral_tokenizer.h
voxtral_kernels.o: voxtral_kernels.c voxtral_kernels.h
voxtral_audio.o: voxtral_audio.c voxtral_audio.h
voxtral_encoder.o: voxtral_encoder.c voxtral.h voxtral_kernels.h voxtral_safetensors.h
voxtral_decoder.o: voxtral_decoder.c voxtral.h voxtral_kernels.h voxtral_safetensors.h
voxtral_tokenizer.o: voxtral_tokenizer.c voxtral_tokenizer.h
voxtral_safetensors.o: voxtral_safetensors.c voxtral_safetensors.h
main.o: main.c voxtral.h voxtral_kernels.h voxtral_mic.h
voxtral_mic_macos.o: voxtral_mic_macos.c voxtral_mic.h
voxtral_hotkey_macos.o: voxtral_hotkey_macos.c voxtral_hotkey.h
voxtral_paste_macos.o: voxtral_paste_macos.c voxtral_paste.h
inspect_weights.o: inspect_weights.c voxtral_safetensors.h
