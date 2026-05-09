CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -I./include -std=c++11
LDFLAGS = -lm -lz -framework Metal -framework Foundation

SRC_DIR = src
BUILD_DIR = build

SOURCES = $(SRC_DIR)/main.cc \
          $(SRC_DIR)/vector/vector.cc \
          $(SRC_DIR)/parser/parser.cc \
          $(SRC_DIR)/renderer/renderer.cc \
          $(SRC_DIR)/output/output.cc \
          $(SRC_DIR)/shading/shading.cc

MM_SOURCES = $(SRC_DIR)/renderer/gpu_renderer.mm

OBJECTS = $(patsubst $(SRC_DIR)/%.cc,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS += $(patsubst $(SRC_DIR)/%.mm,$(BUILD_DIR)/%.o,$(MM_SOURCES))

TARGET = ray2

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/renderer/gpu_renderer.o: $(SRC_DIR)/renderer/gpu_renderer.mm
	mkdir -p $(BUILD_DIR)/renderer
	$(CXX) $(CXXFLAGS) -ObjC++ -c $< -o $@ -fobjc-arc

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

run: $(TARGET)
	./$(TARGET) scene.json
