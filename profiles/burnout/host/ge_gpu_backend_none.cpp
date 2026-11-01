// GPU backend for builds that use the VCS GE renderer through its bit-exact
// software rasterizer only: every query reports "no GPU", so ge_renderer.cpp
// draws into guest VRAM itself.
#include "ge_gpu_backend.hpp"

namespace vcs {

bool initialize_ge_gpu_backend(std::string &) { return true; }
void shutdown_ge_gpu_backend() noexcept {}

bool ge_gpu_backend_active() noexcept { return false; }
bool ge_gpu_backend_transfer_ready() noexcept { return false; }
bool ge_gpu_backend_graphics_ready() noexcept { return false; }
void ge_gpu_backend_record_draw(const GeGpuDrawDescriptor &) noexcept {}
void ge_gpu_backend_observe_camera(const std::array<float, 12> &, const std::array<float, 16> &,
                                   const std::array<float, 6> &, const std::array<float, 3> &,
                                   const GeGpuDrawDescriptor &, std::uint32_t) noexcept {}
bool ge_gpu_backend_stage_vertices(const GeGpuDrawDescriptor &, std::span<const GeGpuVertex>) noexcept {
    return false;
}
bool ge_gpu_backend_texture_needed(const GeGpuDrawDescriptor &) noexcept { return false; }
void ge_gpu_backend_prepare_texture_keys(GeGpuDrawDescriptor &) noexcept {}
bool ge_gpu_backend_texture_signature_needed(const GeGpuDrawDescriptor &) noexcept { return false; }
bool ge_gpu_backend_is_framebuffer_feedback_texture(const GeGpuDrawDescriptor &) noexcept { return false; }
GeGpuWidescreenHud ge_gpu_backend_widescreen_hud(const GeGpuDrawDescriptor &) noexcept { return {}; }
void ge_gpu_backend_note_through_extent(const GeGpuDrawDescriptor &, float, float) noexcept {}
bool ge_gpu_backend_adopt_shared_texture(const GeGpuDrawDescriptor &) noexcept { return false; }
bool ge_gpu_backend_texture_available(const GeGpuDrawDescriptor &) noexcept { return false; }
bool ge_gpu_backend_upload_decoded_texture(const GeGpuDrawDescriptor &, std::uint32_t, std::uint32_t,
                                           std::span<const std::byte>) noexcept {
    return false;
}
bool ge_gpu_backend_upload_decoded_texture_chain(const GeGpuDrawDescriptor &,
                                                 std::span<const GeGpuDecodedMipLevel>) noexcept {
    return false;
}
bool ge_gpu_backend_upload_decoded_texture_chain_packed(const GeGpuDrawDescriptor &, std::uint32_t, std::uint32_t,
                                                        std::uint32_t, std::vector<std::byte>) noexcept {
    return false;
}
bool ge_gpu_backend_copy_last_texture_rgba(std::span<std::byte>) noexcept { return false; }
void ge_gpu_backend_accumulate_color_triangles(const GeGpuDrawDescriptor &, std::span<const GeGpuVertex>) noexcept {}
void ge_gpu_backend_accumulate_hardware_triangles(const GeGpuDrawDescriptor &, const GeGpuHardwareTransform &,
                                                  std::span<const GeGpuVertex>,
                                                  std::span<const std::uint32_t>) noexcept {}
bool ge_gpu_backend_accumulate_hardware_packed_0115(const GeGpuDrawDescriptor &, const GeGpuHardwareTransform &,
                                                    std::span<const std::byte>, std::uint32_t,
                                                    std::span<const std::uint32_t>) noexcept {
    return false;
}
void ge_gpu_backend_set_native_window(void *) noexcept {}
void ge_gpu_backend_set_display_framebuffer(std::uint32_t) noexcept {}
bool ge_gpu_backend_finish_color_frame(std::uint64_t) noexcept { return false; }
bool ge_gpu_backend_copy_game_frame_rgba(std::span<std::byte>) noexcept { return false; }
bool ge_gpu_backend_presents_directly() noexcept { return false; }
std::uint32_t ge_gpu_backend_owned_framebuffer() noexcept { return 0u; }
std::uint32_t ge_gpu_backend_display_framebuffer() noexcept { return 0u; }
std::span<const std::byte> ge_gpu_backend_game_frame_rgba() noexcept { return {}; }
bool ge_gpu_backend_copy_offscreen_rgba(std::span<std::byte>) noexcept { return false; }
void ge_gpu_backend_mark_window_presented() noexcept {}
GeGpuBackendReport ge_gpu_backend_report() { return {}; }
const char *ge_gpu_backend_name(GeGpuBackendKind) noexcept { return "software"; }

} // namespace vcs

// vcs_project2dfx.cpp references the VCS draw-distance patch; it never runs here.
#include "psprecomp/runtime.hpp"

#include <filesystem>

namespace vcs {
void install_draw_distance_patch(psprecomp::Runtime &, const std::filesystem::path &) {}
} // namespace vcs
