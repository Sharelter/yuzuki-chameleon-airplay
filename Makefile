BUILD_DIR ?= build
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all check
all: $(BUILD_DIR)/nowplaying $(BUILD_DIR)/yuzuki-admin $(BUILD_DIR)/fbmode

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/nowplaying: nowplaying/nowplaying.c third_party/stb_image.h third_party/stb_truetype.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" -lm $(LDLIBS)

$(BUILD_DIR)/yuzuki-admin: webadmin/yuzuki_admin.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(LDLIBS)

$(BUILD_DIR)/fbmode: nowplaying/fbmode.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" "$<" $(LDLIBS)

check: all
	@set -e; for script in board/*.init nowplaying/*.init webadmin/*.init webadmin/hdmi-audio-recovery; do \
		sh -n "$$script"; \
	done
