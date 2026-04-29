#include <stdio.h>
#include "parser/parser.h"
#include "renderer/renderer.h"
#include "output/output.h"

int main(int argc, char** argv) {
    const char* scene_file = "scene.json";
    if (argc > 1) scene_file = argv[1];
    
    Scene* scene = parse_scene(scene_file);
    if (!scene) {
        fprintf(stderr, "Failed to parse scene file: %s\n", scene_file);
        return 1;
    }
    
    Image* img = render_frame(scene);
    
    // Write PPM to stdout (original behavior)
    write_ppm(img, stdout);
    
    // Optionally write PNG if output filename specified
    if (scene->output[0]) {
        write_png(img, scene->output);
        printf("Also saved as %s\n", scene->output);
    }
    
    free_image(img);
    free_scene(scene);
    return 0;
}
