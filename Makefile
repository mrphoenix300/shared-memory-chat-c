CC = gcc
CFLAGS = -Wall -pthread -g -Iinclude
LDFLAGS = -lrt


BUILD_DIR = build
SRC_DIR = src
INCLUDE_DIR = include


TARGET = $(BUILD_DIR)/chat
SRC = $(SRC_DIR)/main.c
DEPS = $(INCLUDE_DIR)/common.h

all: $(TARGET)

$(TARGET): $(SRC) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run