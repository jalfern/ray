CC = gcc
CFLAGS = -Wall -Wextra -O2 -I./include
LDFLAGS = -lm -lz

SRC_DIR = src
BUILD_DIR = build

SOURCES = $(SRC_DIR)/main.c \
          $(SRC_DIR)/vector/vector.c \
          $(SRC_DIR)/parser/parser.c \
          $(SRC_DIR)/renderer/renderer.c \
          $(SRC_DIR)/output/output.c \
          $(SRC_DIR)/shading/shading.c

OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

TARGET = ray2

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

run: $(TARGET)
	./$(TARGET) scene.json
