#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include "parser/parser.h"
#include "renderer/renderer.h"
#include "renderer/gpu_renderer.h"
#include "output/output.h"

static int use_gpu = 1;
static int num_threads = 0;

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static Image* render_best(const Scene* scene) {
    if (use_gpu) {
        Image* img = render_frame_gpu(scene);
        if (img) {
            printf("GPU  ");
            return img;
        }
        printf("GPU unavailable, falling back to CPU\n");
        use_gpu = 0;
    }
    int n = num_threads > 0 ? num_threads : (int)std::thread::hardware_concurrency();
    if (n < 1) n = 1;
    printf("%dT  ", n);
    return render_frame_parallel(scene, n);
}

static void render_animation(Scene* scene) {
    int total_frames = (int)(scene->animation.duration * scene->animation.fps);
    if (total_frames < 1) return;

    mkdir("frames", 0755);

    Vec3 target = scene->animation.orbit_center;
    double anim_start = now_sec();
    float* frame_times = new float[total_frames];
    float t_min = 1e9f, t_max = 0.0f, t_sum = 0.0f;

    for (int i = 0; i < total_frames; i++) {
        float angle = 2.0f * (float)M_PI * i / total_frames;
        scene->camera_pos.x = target.x + scene->animation.orbit_radius * cosf(angle);
        scene->camera_pos.y = scene->animation.orbit_height;
        scene->camera_pos.z = target.z + scene->animation.orbit_radius * sinf(angle);
        scene->camera_target = target;

        double t0 = now_sec();
        Image* img = render_best(scene);
        double t1 = now_sec();

        char filename[64];
        snprintf(filename, sizeof(filename), "frames/frame_%04d.png", i);
        write_png(img, filename);
        free_image(img);

        float dt = (float)(t1 - t0);
        frame_times[i] = dt;
        if (dt < t_min) t_min = dt;
        if (dt > t_max) t_max = dt;
        t_sum += dt;

        printf("\rFrame %4d/%d  %5.1fms  avg %5.1fms  eta %5.0fs  ",
               i + 1, total_frames, dt * 1000, (t_sum / (i + 1)) * 1000,
               (t_sum / (i + 1)) * (total_frames - i - 1));
        fflush(stdout);
    }

    double total_sec = now_sec() - anim_start;
    printf("\nDone. %d frames in %.1fs (avg %.0fms, min %.0fms, max %.0fms)\n",
           total_frames, total_sec, t_sum / total_frames * 1000,
           t_min * 1000, t_max * 1000);

    delete[] frame_times;

    // Convert to video
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -framerate %d -i frames/frame_%%04d.png "
        "-c:v libx264 -pix_fmt yuv420p -preset medium -crf 18 %s 2>/dev/null",
        scene->animation.fps, scene->output[0] ? scene->output : "output.mp4");
    printf("Converting to video...\n");
    int ret = system(cmd);
    if (ret == 0) {
        printf("Video saved as %s\n", scene->output[0] ? scene->output : "output.mp4");
    } else {
        fprintf(stderr, "ffmpeg conversion failed (exit code %d)\n", ret);
    }
}

int main(int argc, char** argv) {
    const char* scene_file = "scenes/scene.json";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cpu") == 0) use_gpu = 0;
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
            if (num_threads < 1) num_threads = 1;
        }
        else if (argv[i][0] != '-') scene_file = argv[i];
    }
    
    Scene* scene = parse_scene(scene_file);
    if (!scene) {
        fprintf(stderr, "Failed to parse scene file: %s\n", scene_file);
        return 1;
    }

    if (scene->has_animation) {
        render_animation(scene);
        free_scene(scene);
        return 0;
    }
    
    Image* img = render_best(scene);
    
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
