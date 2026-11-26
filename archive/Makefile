CC ?= clang
CFLAGS ?= -Wall -Wextra -std=c99 -Iinclude -Isrc -Ivendor -pthread -g
LDFLAGS ?= -pthread -lcurl -lsqlite3

# Cross-compilation variables for PS5 sie-ps5 ELF
LLD_PATH ?= $(shell (which ld.lld 2>/dev/null || ls /opt/homebrew/Cellar/llvm*/*/bin/ld.lld /opt/homebrew/opt/llvm*/bin/ld.lld /usr/local/bin/ld.lld /usr/bin/ld.lld 2>/dev/null) | head -n 1)
LLVM_DIR := $(if $(LLD_PATH),$(dir $(LLD_PATH)),)
CLANG_PS5 ?= $(if $(wildcard $(LLVM_DIR)clang),$(LLVM_DIR)clang,clang)
LLVM_STRIP ?= $(if $(wildcard $(LLVM_DIR)llvm-strip),$(LLVM_DIR)llvm-strip,$(shell (which llvm-strip 2>/dev/null || which strip 2>/dev/null) | head -n 1))

PS5_CFLAGS ?= -target x86_64-sie-ps5 -O2 -fPIC -DPS5 -fno-stack-protector -fno-plt -femulated-tls -I./include -I./src -I./vendor -isystem vendor/ps5-sdk/include/freebsd -isystem vendor/ps5-sdk/include -Wno-unreachable-code-generic-assoc -include sys/time.h -include unistd.h

PS5_LDFLAGS ?=
ifneq ($(LLD_PATH),)
PS5_LDFLAGS += -fuse-ld=$(LLD_PATH) -Wl,-pie vendor/ps5-sdk/lib/crt1.o -Lvendor/ps5-sdk/lib -lc -lkernel -lSceLibcInternal -lSceAppInstUtil -lSceSystemService -lSceUserService -lSceNetCtl -lSceHttp -lSceSsl -lSceNet
endif

BIN_DIR := bin
SRC_DIR := src
VENDOR_DIR := vendor
TESTS_DIR := tests

all: test $(BIN_DIR)/ps5-youtube-host

test: test_version test_config test_crypto test_youtube test_watcher test_server test_web test_media_tile test_shortcut test_e2e test_payload_repo

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

web_assets: $(SRC_DIR)/web_assets.h

$(SRC_DIR)/web_assets.h: web/index.html web/style.css web/app.js scripts/bundle_web.py
	python3 scripts/bundle_web.py

shortcut_assets: $(SRC_DIR)/shortcut_assets.h

dist/shortcut/PS5YT0001/sce_sys/param.json dist/shortcut/PS5YT0001/sce_sys/icon0.png: scripts/generate_shortcut.py
	python3 scripts/generate_shortcut.py

$(SRC_DIR)/shortcut_assets.h: dist/shortcut/PS5YT0001/sce_sys/param.json dist/shortcut/PS5YT0001/sce_sys/icon0.png scripts/bundle_shortcut.py
	python3 scripts/bundle_shortcut.py

$(BIN_DIR)/ps5-youtube-host: $(BIN_DIR) $(SRC_DIR)/web_assets.h $(SRC_DIR)/shortcut_assets.h $(wildcard $(SRC_DIR)/*.c) $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $(filter %.c, $^) -o $@ $(LDFLAGS)

PS5_BUILD_DIR := $(BIN_DIR)/ps5-obj
PS5_SRCS := $(wildcard $(SRC_DIR)/*.c) $(VENDOR_DIR)/cJSON.c
PS5_OBJS := $(patsubst $(SRC_DIR)/%.c,$(PS5_BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%.c,$(PS5_SRCS))) $(PS5_BUILD_DIR)/cJSON.o

$(PS5_BUILD_DIR):
	mkdir -p $(PS5_BUILD_DIR)

$(PS5_BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/web_assets.h $(SRC_DIR)/shortcut_assets.h | $(PS5_BUILD_DIR)
	$(CLANG_PS5) $(PS5_CFLAGS) -c $< -o $@

$(PS5_BUILD_DIR)/cJSON.o: $(VENDOR_DIR)/cJSON.c | $(PS5_BUILD_DIR)
	$(CLANG_PS5) $(PS5_CFLAGS) -c $< -o $@

ps5: $(BIN_DIR) $(SRC_DIR)/web_assets.h $(SRC_DIR)/shortcut_assets.h $(PS5_OBJS)
	@if [ -z "$(LLD_PATH)" ]; then \
		echo "Error: ld.lld is required for PS5 ELF cross-compilation. Please install lld (e.g. brew install lld)." >&2; \
		exit 1; \
	fi
	$(LLD_PATH) -pie -T vendor/ps5-sdk/ldscripts/elf_x86_64.x \
		-z max-page-size=0x4000 --eh-frame-hdr -mllvm -emulated-tls --hash-style=gnu \
		vendor/ps5-sdk/lib/crt1.o $(PS5_OBJS) \
		-Lvendor/ps5-sdk/lib -lc -lkernel -lSceLibcInternal -lSceAppInstUtil \
		-lSceSystemService -lSceUserService -lSceNetCtl -lSceHttp -lSceSsl -lSceNet \
		-o $(BIN_DIR)/ps5-yt-uploader.elf
	@if [ -n "$(LLVM_STRIP)" ]; then $(LLVM_STRIP) $(BIN_DIR)/ps5-yt-uploader.elf 2>/dev/null || true; fi
	@ln -sf ps5-yt-uploader.elf $(BIN_DIR)/ps5-youtube.elf
	@ln -sf ps5-yt-uploader.elf $(BIN_DIR)/payload.elf
	@echo "Built $(BIN_DIR)/ps5-yt-uploader.elf size: $$(ls -lh $(BIN_DIR)/ps5-yt-uploader.elf | awk '{print $$5}')"

$(BIN_DIR)/test_version: $(TESTS_DIR)/test_version_cli.c $(SRC_DIR)/version.h
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

$(BIN_DIR)/test_server: $(TESTS_DIR)/test_server_api.c $(SRC_DIR)/server.c $(SRC_DIR)/api.c $(SRC_DIR)/watcher.c $(SRC_DIR)/notify.c $(SRC_DIR)/youtube.c $(SRC_DIR)/http_client.c $(SRC_DIR)/crypto.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c $(SRC_DIR)/web_assets.h $(SRC_DIR)/version.h
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

test_shortcut:
	python3 tests/test_shortcut_generator.py

shortcut:
	python3 scripts/generate_shortcut.py

test_e2e: $(BIN_DIR)/ps5-youtube-host
	bash tests/test_e2e_integration.sh

test_payload_repo:
	python3 tests/test_payload_repo.py

release:
	bash scripts/release.sh

clean:
	rm -rf $(BIN_DIR) dist

.PHONY: all test test_version test_config test_crypto test_youtube test_watcher test_server test_web test_media_tile test_shortcut shortcut test_e2e test_payload_repo release ps5 web_assets clean
