CC ?= clang
CFLAGS ?= -Wall -Wextra -std=c99 -Iinclude -Isrc -Ivendor -pthread -g
LDFLAGS ?= -pthread -lcurl


BIN_DIR := bin
SRC_DIR := src
VENDOR_DIR := vendor
TESTS_DIR := tests

all: test_config test_crypto test_youtube test_watcher test_server test_web

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

web_assets: $(SRC_DIR)/web_assets.h

$(SRC_DIR)/web_assets.h: web/index.html web/style.css web/app.js scripts/bundle_web.py
	python3 scripts/bundle_web.py

test_config: $(BIN_DIR) $(TESTS_DIR)/test_config.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $(TESTS_DIR)/test_config.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c -o $(BIN_DIR)/test_config $(LDFLAGS)

test_crypto: $(BIN_DIR) $(TESTS_DIR)/test_crypto.c $(SRC_DIR)/crypto.c
	$(CC) $(CFLAGS) $(TESTS_DIR)/test_crypto.c $(SRC_DIR)/crypto.c -o $(BIN_DIR)/test_crypto $(LDFLAGS)

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

clean:
	rm -rf $(BIN_DIR)

.PHONY: all test_config test_crypto test_youtube test_watcher test_server test_web web_assets clean
