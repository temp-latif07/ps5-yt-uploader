CC ?= clang
CFLAGS ?= -Wall -Wextra -std=c99 -Iinclude -Isrc -Ivendor -pthread -g
LDFLAGS ?= -pthread


BIN_DIR := bin
SRC_DIR := src
VENDOR_DIR := vendor
TESTS_DIR := tests

all: test_config test_crypto

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

test_config: $(BIN_DIR) $(TESTS_DIR)/test_config.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c
	$(CC) $(CFLAGS) $(TESTS_DIR)/test_config.c $(SRC_DIR)/config.c $(VENDOR_DIR)/cJSON.c -o $(BIN_DIR)/test_config $(LDFLAGS)

test_crypto: $(BIN_DIR) $(TESTS_DIR)/test_crypto.c $(SRC_DIR)/crypto.c
	$(CC) $(CFLAGS) $(TESTS_DIR)/test_crypto.c $(SRC_DIR)/crypto.c -o $(BIN_DIR)/test_crypto $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)

.PHONY: all test_config test_crypto clean
