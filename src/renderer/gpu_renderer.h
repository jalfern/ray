#ifndef GPU_RENDERER_H
#define GPU_RENDERER_H

#include "../../include/scene.h"
#include "../output/output.h"

// Renders a frame on the GPU. Returns NULL when no GPU backend is available,
// in which case the caller falls back to the CPU renderer.
Image* render_frame_gpu(const Scene* scene);

// Valid after render_frame_gpu returned NULL: the failed command buffer's
// status (-1 = GPU failed before dispatch, e.g. init) and its localized
// error description ("" if none).
long gpu_fail_status(void);
const char* gpu_fail_error(void);

#endif
