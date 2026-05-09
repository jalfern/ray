#ifndef GPU_RENDERER_H
#define GPU_RENDERER_H

#include "../../include/scene.h"
#include "../output/output.h"

Image* render_frame_gpu(const Scene* scene);

#endif
