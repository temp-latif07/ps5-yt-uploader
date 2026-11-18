CC ?= clang
CFLAGS ?= -Wall -Wextra -std=c99 -Iinclude -Isrc -Ivendor -pthread -g
LDFLAGS ?= -pthread -lcurl -lsqlite3

# Cross-compilation variables for PS5 FreeBSD 12 ELF
LLD_PATH ?= $(shell which ld.lld 2>/dev/null || ls /opt/homebrew/bin/ld.lld /opt/homebrew/opt/llvm*/bin/ld.lld /usr/local/bin/ld.lld /usr/bin/ld.lld 2>/dev/null | head -n 1)
LLVM_STRIP ?= $(shell which llvm-strip 2>/dev/null || ls /opt/homebrew/bin/llvm-strip /opt/homebrew/opt/llvm*/bin/llvm-strip /usr/local/bin/llvm-strip 2>/dev/null | head -n 1 || which strip 2>/dev/null)
MACOS_SDK ?= $(shell xcrun --show-sdk-path 2>/dev/null)

PS5_CFLAGS ?= -target x86_64-unknown-freebsd12 -O2 -fPIC -DPS5 -I./include -I./src -I./vendor
ifneq ($(MACOS_SDK),)
PS5_CFLAGS += -isystem $(MACOS_SDK)/usr/include
endif

PS5_LDFLAGS ?=
ifneq ($(LLD_PATH),)
PS5_LDFLAGS += -fuse-ld=$(LLD_PATH) -nostdlib -Wl,--unresolved-symbols=ignore-all -Wl,-pie -Wl,-e,main
endif

BIN_DIR := bin
SRC_DIR := src
VENDOR_DIR := vendor
TESTS_DIR := tests

all: test $(BIN_DIR)/ps5-youtube-host

test: test_version test_config test_crypto test_youtube test_watcher test_server test_web test_media_tile test_e2e test_payload_repo

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

web_assets: $(SRC_DIR)/web_assets.h

$(SRC_DIR)/web_assets.h: web/index.html web/style.css web/app.js scripts/bundle_web.py
	python3 scripts/bundle_web.py

$(BIN_DIR)/ps5-youtube-host: $(BIN_DIR) $(SRC_DIR)/web_assets.h $(wildcard $(SRC_DIR)/*.c) $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $(filter %.c, $^) -o $@ $(LDFLAGS)

ps5: $(BIN_DIR) $(SRC_DIR)/web_assets.h $(wildcard $(SRC_DIR)/*.c) $(VENDOR_DIR)/cJSON.c
	@if [ -z "$(LLD_PATH)" ]; then \
		echo "Error: ld.lld is required for PS5 ELF cross-compilation. Please install lld (e.g. brew install lld)." >&2; \
		exit 1; \
	fi
	$(CC) $(PS5_CFLAGS) $(wildcard $(SRC_DIR)/*.c) $(VENDOR_DIR)/cJSON.c -o $(BIN_DIR)/ps5-yt-uploader.elf $(PS5_LDFLAGS)
	@if [ -n "$(LLVM_STRIP)" ]; then $(LLVM_STRIP) $(BIN_DIR)/ps5-yt-uploader.elf 2>/dev/null || true; fi
	@ln -sf ps5-yt-uploader.elf $(BIN_DIR)/ps5-youtube.elf
	@echo "Built $(BIN_DIR)/ps5-yt-uploader.elf size: $$(ls -lh $(BIN_DIR)/ps5-yt-uploader.elf | awk '{print $$5}')"

$(BIN_DIR)/test_version: $(TESTS_DIR)/test_version_cli.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

test_version: $(BIN_DIR)/ps5-youtube-host $(BIN_DIR)/test_version
	./$(BIN_DIR)/test_version

test_config: $(BIN_DIR) $(TESTS_DIR)/test_config.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $(TESTS_DIR)/test_config.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c -o $(BIN_DIR)/test_config $(LDFLAGS)
	./$(BIN_DIR)/test_config

test_crypto: $(BIN_DIR) $(TESTS_DIR)/test_crypto.c $(SRC_DIR)/crypto.c
	$(CC) $(CFLAGS) $(TESTS_DIR)/test_crypto.c $(SRC_DIR)/crypto.c -o $(BIN_DIR)/test_crypto $(LDFLAGS)
	./$(BIN_DIR)/test_crypto

$(BIN_DIR)/test_youtube: $(TESTS_DIR)/test_youtube_upload.c $(SRC_DIR)/youtube.c $(SRC_DIR)/http_client.c $(SRC_DIR)/crypto.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_youtube: $(BIN_DIR) $(BIN_DIR)/test_youtube
	@python3 tests/mock_youtube_server.py 8999 > /dev/null 2>&1 & \
	SERVER_PID=$$!; \
	sleep 0.3; \
	./$(BIN_DIR)/test_youtube; \
	TEST_EXIT=$$?; \
	kill $$SERVER_PID 2>/dev/null || true; \
	wait $$SERVER_PID 2>/dev/null || true; \
	exit $$TEST_EXIT

$(BIN_DIR)/test_watcher: $(TESTS_DIR)/test_watcher.c $(SRC_DIR)/watcher.c $(SRC_DIR)/notify.c $(SRC_DIR)/youtube.c $(SRC_DIR)/http_client.c $(SRC_DIR)/crypto.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_watcher: $(BIN_DIR) $(BIN_DIR)/test_watcher
	./$(BIN_DIR)/test_watcher

$(BIN_DIR)/test_server: $(TESTS_DIR)/test_server_api.c $(SRC_DIR)/server.c $(SRC_DIR)/api.c $(SRC_DIR)/watcher.c $(SRC_DIR)/notify.c $(SRC_DIR)/youtube.c $(SRC_DIR)/http_client.c $(SRC_DIR)/crypto.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c $(SRC_DIR)/web_assets.h
	$(CC) $(CFLAGS) $(filter %.c, $^) -o $@ $(LDFLAGS)

test_server: $(BIN_DIR) $(BIN_DIR)/test_server
	./$(BIN_DIR)/test_server

$(BIN_DIR)/test_web: $(TESTS_DIR)/test_web_assets.c $(SRC_DIR)/server.c $(SRC_DIR)/api.c $(SRC_DIR)/watcher.c $(SRC_DIR)/notify.c $(SRC_DIR)/youtube.c $(SRC_DIR)/http_client.c $(SRC_DIR)/crypto.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c $(SRC_DIR)/web_assets.h
	$(CC) $(CFLAGS) $(filter %.c, $^) -o $@ $(LDFLAGS)

test_web: $(BIN_DIR) $(SRC_DIR)/web_assets.h $(BIN_DIR)/test_web
	./$(BIN_DIR)/test_web

$(BIN_DIR)/test_media_tile: $(TESTS_DIR)/test_media_tile.c $(SRC_DIR)/media_tile.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_media_tile: $(BIN_DIR) $(BIN_DIR)/test_media_tile
	./$(BIN_DIR)/test_media_tile

test_e2e: $(BIN_DIR)/ps5-youtube-host
	bash tests/test_e2e_integration.sh

test_payload_repo:
	python3 tests/test_payload_repo.py

release:
	bash scripts/release.sh

clean:
	rm -rf $(BIN_DIR)

.PHONY: all test test_version test_config test_crypto test_youtube test_watcher test_server test_web test_media_tile test_e2e test_payload_repo release ps5 web_assets clean
