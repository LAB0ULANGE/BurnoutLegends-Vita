// vitaGL context for the PS Vita GE renderer: 960x544, shaders compiled at
// run time by vitaShaRK (libshacccg.suprx must be installed).
#include "gl_api.hpp"

namespace burnout::gl {

bool create_context(std::string &error) {
    // Leave most of the RAM to the guest heap; GL resources live in CDRAM first.
    if (!vglInitExtended(0, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE)) {
        error = "vglInitExtended failed";
        return false;
    }
    return true;
}

void swap_buffers() { vglSwapBuffers(GL_FALSE); }

} // namespace burnout::gl
