CC ?= clang
CFLAGS ?= -Wall -Wextra -std=c99 -Iinclude -Isrc -Ivendor -pthread -g
LDFLAGS ?= -pthread -lcurl


BIN_DIR := bin
SRC_DIR := src
VENDOR_DIR := vendor
TESTS_DIR := tests

all: test_config test_crypto test_youtube test_watcher

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

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

clean:
	rm -rf $(BIN_DIR)

.PHONY: all test_config test_crypto test_youtube test_watcher clean
