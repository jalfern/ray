#include "gpu_renderer.h"

// No GPU backend is currently wired up. Returning NULL signals main() to fall
// back to the multithreaded CPU renderer (see render_best() in main.cc).
//
// A previous attempt at a Metal compute backend lived here but was never
// functional (it loaded its shader from an app bundle that a CLI binary does
// not have, so it always returned NULL anyway). It has been removed in favor
// of this honest stub. The CPU renderer is the real rendering engine.
Image* render_frame_gpu(const Scene* scene) {
    (void)scene;
    return NULL;
}
