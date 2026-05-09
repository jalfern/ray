#ifndef RENDERER_H
#define RENDERER_H

#include "../../include/scene.h"
#include "../output/output.h"
#include "../../include/types.h"
#include "../shading/shading.h"

Image* render_frame(const Scene* scene);
Image* render_frame_parallel(const Scene* scene, int num_threads);

#endif
