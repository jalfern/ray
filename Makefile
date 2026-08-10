CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -I./include -std=c++11
MMFLAGS = -Wall -Wextra -O2 -I./include -I$(BUILD_DIR)/renderer -std=c++11 -fobjc-arc
LDFLAGS = -lm -lz -framework Metal -framework Foundation

SRC_DIR = src
BUILD_DIR = build
TOOLS_DIR = tools
TOOLS_BUILD = $(BUILD_DIR)/tools

SOURCES = $(SRC_DIR)/main.cc \
          $(SRC_DIR)/vector/vector.cc \
          $(SRC_DIR)/parser/parser.cc \
          $(SRC_DIR)/parser/obj_parser.cc \
          $(SRC_DIR)/parser/gltf_parser.cc \
          $(SRC_DIR)/renderer/renderer.cc \
          $(SRC_DIR)/renderer/bvh.cc \
          $(SRC_DIR)/output/output.cc \
          $(SRC_DIR)/shading/shading.cc \
          $(SRC_DIR)/denoiser/denoiser.cc \
          $(SRC_DIR)/envmap/envmap.cc

MM_SOURCES = $(SRC_DIR)/renderer/gpu_renderer.mm

OBJECTS = $(patsubst $(SRC_DIR)/%.cc,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS += $(patsubst $(SRC_DIR)/%.mm,$(BUILD_DIR)/%.o,$(MM_SOURCES))

SHADER_SRC = $(SRC_DIR)/renderer/shaders.metal
SHADER_HDR = $(BUILD_DIR)/renderer/shader_src.h

TARGET = ray2

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Embed the Metal shader source as a C string compiled at runtime (no `metal`
# CLI / Xcode needed — newLibraryWithSource uses the framework's compiler).
$(SHADER_HDR): $(SHADER_SRC)
	mkdir -p $(dir $@)
	printf 'static const char* kShaderSource = R"METALSHADER(\n' > $@
	cat $< >> $@
	printf '\n)METALSHADER";\n' >> $@

$(BUILD_DIR)/renderer/gpu_renderer.o: $(SRC_DIR)/renderer/gpu_renderer.mm $(SHADER_HDR)
	mkdir -p $(dir $@)
	$(CXX) $(MMFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TOOLS_BUILD)

models: $(TOOLS_BUILD)/gen_torus $(TOOLS_BUILD)/gen_ico $(TOOLS_BUILD)/gen_vase
	$(TOOLS_BUILD)/gen_torus 3 1 32 20 > models/torus.obj
	$(TOOLS_BUILD)/gen_ico 2.5 > models/ico.obj
	$(TOOLS_BUILD)/gen_vase 32 > models/vase.obj

$(TOOLS_BUILD)/gen_torus: $(TOOLS_DIR)/gen_torus.c
	mkdir -p $(TOOLS_BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ -lm

$(TOOLS_BUILD)/gen_ico: $(TOOLS_DIR)/gen_ico.c
	mkdir -p $(TOOLS_BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ -lm

$(TOOLS_BUILD)/gen_vase: $(TOOLS_DIR)/gen_vase.c
	mkdir -p $(TOOLS_BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@ -lm

test: $(TARGET)
	@echo "Rendering torus..."
	@./$(TARGET) scenes/scene_torus.json > /dev/null
	@echo "Rendering ico..."
	@./$(TARGET) scenes/scene_ico.json > /dev/null
	@echo "Rendering vase..."
	@./$(TARGET) scenes/scene_vase.json > /dev/null
	@echo "Rendering demo..."
	@./$(TARGET) scenes/scene_demo.json > /dev/null
	@echo "Rendering emissive demo..."
	@./$(TARGET) scenes/scene_emissive_demo.json > /dev/null
	@echo "Rendering complex (65K tri torus)..."
	@./$(TARGET) scenes/scene_complex.json > /dev/null
	@echo "All done:"
	@ls -lh images/ videos/ 2>/dev/null

run: $(TARGET)
	./$(TARGET) scenes/scene.json
