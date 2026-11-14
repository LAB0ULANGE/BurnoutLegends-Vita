#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace burnout::gl {

// GE display lists translated to OpenGL ES 2 (vitaGL on the PS Vita, desktop
// GL on Windows for validation). Vertices keep their PSP formats; the vertex
// shader converts them and does the transform, PSP lighting and texture-
// coordinate generation. Consecutive PRIMs that share all state are drawn as
// one batch.
// Every PSP render target becomes a framebuffer object; a texture whose
// address is a render target samples that object.

// Starts the GL thread, which creates the context and replays what the draw
// path records (see ge_gl.cpp).
bool initialize(std::string &error);
// Internal resolution as a multiple of the PSP's 480x272 (before initialize).
void set_render_scale(float scale);
// Vita: sharpen the picture as it is enlarged to the screen (render scale below 2).
void set_lazy_levels(bool enabled);
// Vita: compiled shaders kept in this directory (empty: none).
void set_gxp_cache(const std::string &directory);
// Static geometry: its vertices hashed once in `frames` frames (1: every frame).
void set_probe_every(std::uint32_t frames);
void set_effect_1x(bool enabled);  // gl-95 test
// gl-97: GPU scenes limited to the columns each target's draws reach (Vita, patched vitaGL).
void set_tight_regions(bool enabled);
// gl-89: dynamic resolution in one target per screen buffer, GPU tiles limited to the drawn part.
void set_valid_region(bool enabled);
// Vita: GXM parameter buffer size before initialize (0: vitaGL default 16 MiB).
void set_param_buffer_mib(int mib);
void set_large_rings(bool large);  // Vita: GXM command rings enlarged (gl_context_vita.cpp)
// Vita: GXM scenes per frame of the FBO render targets (vitaGL; before initialize).
void set_fbo_scenes(int scenes);
// gl-90: GPU probe on every draw (log mode 100 normal, 101 32x32 scissor, 102 white texture, 103 no blending).
void set_all_probe(bool enabled);
// gl-91: probe set 2 (log 200 normal, 201 no draws, 202 no effect-buffer draws, 203 present without quad).
void set_probe_set(int set);
void set_only_current_level(bool enabled);  // test
void set_present_sharpen(bool enabled);
// Bonus filters (indices into filters::all(), at most 3): applied by the
// present pass from the next frame; empty = the plain present pass.
void set_filters(const std::vector<int> &active);
// While a menu is up (front end, pause) the picture is shown without the
// filters (they made the menus unreadable); races keep them.
void set_filters_suspended(bool suspended);
// The game's 3D draws left out (2D, through mode, kept): the end-of-race
// Signature award, where two pictures replace the turning car.
void set_hide_3d(bool hide);
// Directory where the filters' programs are kept once built (Vita).
void set_filter_shader_cache(const std::string &directory);
// The Bonus menu's postcard: its photo through these filters, drawn in the box
// (centre x, y from the top, width, height on the 960x544 screen, turned by
// angle radians); empty: none. The photo is the race beside the menu, taken
// at the next frame after postcard_photo() (the live picture until then).
void set_postcard(const std::vector<int> &filters, float x, float y, float width, float height, float angle);
void postcard_photo();
// The sun's glare in races (its additive 2D draws' alpha scaled; 1: as the game).
void set_sun_glare(float scale);
// Ready-made postcards: <directory>/<filter id>.jpg shown as they are.
void set_postcard_pictures(const std::string &directory);
// The presented picture (PC build): its size, 960x544 by default (the Vita).
void set_present_size(int width, int height);
// A PNG (with its transparency) over the screen: centre x, y in PSP pixels
// (480x272, y from the top), size, angle (radians); path empty: none.
void set_photo(const std::string &path, float x, float y, float width, float height, float angle);
// The title logo, a PNG over the screen in a slot of its own: centre x, top y
// in PSP pixels, width (height from the picture); path empty: none.
void set_title_logo(const std::string &path, float x, float top, float width);
// Picture slot 0-3 (the Bonus 2.0 pages): a PNG or JPG over the screen, centre
// x, y, width, height in 960x544 pixels, angle (radians); path empty: none.
void set_picture(int slot, const std::string &path, float x, float y, float width, float height, float angle);
bool picture_ready(int slot);  // its file decoded and drawn (or no picture in the slot)
// PC tool: every filter applied to this picture, written to the directory as
// <filter id>.ppm at the next capture.
void make_postcards(const std::string &picture, const std::string &directory);
// gl-77: foliage PRIMs past near_distance partly left out (2 in 3 kept), past far_distance more (1 in 3 kept).
void set_thin_foliage(bool enabled, float near_distance, float far_distance);
// Screen-sized targets drawn between min_scale and the render scale, by GPU load.
void set_dynamic_resolution(bool enabled, float min_scale, float pinned = 0.0f);
// The picture drawn at this scale from the next frame (1 = the PSP's 480x272),
// 0: the dynamic resolution again. Needs the dynamic resolution on.
void set_picture_scale(float scale);
// Render targets narrower than the screen (the game's effect buffers) at 1x
// whatever the render scale (before initialize).
void set_offscreen_1x(bool enabled);
// Diagnostic: the stencil test off every other 600 frames (GPU cost A/B).
void set_stencil_ab(bool enabled);
// Diagnostic: GPU probe modes rotating every 600 frames (see ge_gl.cpp).
void set_gpu_probe(bool enabled);
// Foliage (cut-out, mostly green scenery textures: trees, bushes) is not drawn.
void set_no_trees(bool enabled);
// gl-72: cut-outs drawn depth-only with discard, then in colour at EQUAL depth without it.
void set_cutout_prepass(bool enabled);
// gl-74 test: draws past this many in a frame are dropped (0: none).
void set_draw_budget(unsigned budget);
// Test: alpha-tested opaque draws after the opaque ones (punch-through last).
void set_reorder_cutouts(bool enabled);
// Test: unlit cut-out PRIMs transformed to world space as decoded, so they merge into few draws.
void set_merge_cutouts(bool enabled, bool all = false);
// Test: those deferred cut-outs blend with a 0/1 alpha instead of discarding.
void set_blend_deferred_cutouts(bool enabled);
// Test: variant fragment shaders declared at medium precision.
void set_fragment_mediump(bool enabled);
// Vita, before initialize: where compiled programs are kept (empty: none),
// and whether vitaGL's pools use cached memory.
void set_shader_cache(const std::string &directory);
void set_cached_memory(bool cached);
// File where program variants first needed while playing are kept, so the
// next launch compiles them up front (empty: none).
void set_variant_list(const std::string &path);
[[nodiscard]] bool cached_memory() noexcept;
// Waits until the GL thread has replayed everything recorded so far.
void finish();

// What the worker is doing, for a sampling thread (bring-up profiling):
// 0 other, 1-5 PRIM (setup, indices, checks, vertex copy, index emit),
// 6-10 draw (state, texture, state, attributes, glDrawElements), 11 present,
// 12 waiting for work, 13 block transfer.
extern volatile std::uint32_t phase;
// Same for the emulation thread: 0 game code, 1 ge_scan, 2 render hook,
// 3 PRIM index snapshot, 4 vertex snapshot, 5 palette snapshot, 6 block
// transfer snapshot, 7 waiting for the worker.
extern volatile std::uint32_t emulation_phase;
// File reads on the emulation thread (sceIoRead*), for the pacing trace.
extern double io_read_ms, io_read_max_ms;
// One line in the log, flushed at once (bring-up on the Vita).
void trace(const char *message);
// FXAA on the race frames (menus, drawn in 2D only, are left sharp).
void set_antialias(bool enabled);
void set_trilinear(bool enabled);
// The light 5-tap anti-aliasing instead of FXAA (before the renderer starts).
void set_antialias_lite(bool lite);
// The player's car speed (mph), for the filters that use it (Chameleon).
void set_car_speed(float mph);
// The next frame's draws listed in the log ([tt] lines), as TARGET_TRACE does.
void trace_next_frame();
// Once the renderer runs, Vita traces are queued and written here (by the
// phase sampler), so no frame waits on the memory card.
void flush_traces();

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset);

// Guest bytes a command list reads, copied on the emulation thread: records
// (PRIM indices, LOAD_CLUT palettes) and PRIM vertices. Vertices are kept
// apart so consecutive PRIMs stay contiguous and can be drawn in place.
struct Snapshot {
    std::vector<std::uint8_t> records, vertices;
};

// Pointer state of the emulation thread, kept apart from the worker's. The
// pointer and format commands only set it (render::forward_run does that in
// place); PRIM and LOAD_CLUT go through snapshot_command.
struct SnapshotState {
    std::uint32_t vtype{}, clut_low{}, clut_high{}, vertex_address{}, index_address{};
};
extern SnapshotState g_scan;

// The GE front end walks a copy of the list's buffer made when the game
// queued it (0.0.37 on the Vita: the game rewrote the buffer mid-walk, a word
// read twice differed and the snapshot records fell out of step). Reads in
// [lo, hi) come from `data`; anything else from guest memory. Set only by the
// front end's thread, around one walk.
struct GuestShadow {
    std::uint32_t lo{}, hi{};
    const std::uint8_t *data{};
};
extern GuestShadow g_shadow;
// Reads of a walk with a copy that went to memory the game rewrites (outside
// the copy and outside 0x08800000-0x09BFFFFF): the copy did not cover them.
extern std::uint64_t g_shadow_misses;
inline const std::uint8_t *shadowed(const psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t size) {
    const std::uint32_t a = address & 0x0FFFFFFFu;
    if (g_shadow.data != nullptr) {
        if (a >= g_shadow.lo && a + size <= g_shadow.hi) return g_shadow.data + (a - g_shadow.lo);
        if (a < 0x08800000u || a >= 0x09C00000u) ++g_shadow_misses;
    }
    return memory.raw_pointer(address, size);
}

// Emulation-thread side of the GL worker. Follows the vertex and index
// pointers and appends to `snapshot` the guest bytes `word` will read, so the
// worker can draw the frame while the game overwrites them. Returns the
// record's offset in `snapshot.records`, or UINT32_MAX when the command needs
// none.
std::uint32_t snapshot_command(const psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base,
                               std::uint32_t offset, Snapshot &snapshot);
// A run of plain commands (see render::ge_commands): PRIM and LOAD_CLUT
// entries take their records from `records` in order.
// Drawing back on after a BJUMP culled an object (the end of a job).
void end_culling();
// Records in the job being drawn (checked against each command's own).
void set_record_count(std::size_t count);
// Whether a job's records ever fell out of step (any thread may ask).
bool records_desynced() noexcept;
// `address`: the guest address of words[0] (0: unknown), for BJUMP.
void ge_run(psprecomp::GuestMemory &memory, const std::uint32_t *words, std::uint32_t count, std::uint32_t base,
            std::uint32_t offset, std::uint32_t address, const Snapshot &snapshot, const std::uint32_t *records,
            std::size_t &next_record);
// Worker side of the same rule: whether the PRIM `word` about to be decoded
// has a record (non-indexed PRIMs of static vertices have none, they are read
// in place).
[[nodiscard]] bool prim_has_record() noexcept;
// The next ge_command reads `record` of `snapshot` instead of guest memory;
// nullptr goes back to guest memory.
void use_snapshot(const Snapshot *snapshot, std::uint32_t record);
// The command list ends and its snapshot goes away: draws what still points
// into it.
void end_list(Snapshot &&snapshot);
// GE block transfer just copied guest memory into [address, address + size).
void invalidate_textures(std::uint32_t address, std::uint32_t size);

// sceDisplaySetFrameBuf: flushes pending draws and shows `address` on the
// Vita screen.
void present(std::uint32_t address);
// Frame skipping: while false, batches are dropped instead of drawn (state,
// palettes and transfers still follow the lists) and present() keeps the
// previous image.
void set_drawing(bool drawing);
// RGB888 copy of the 480x272 visible part of the render target at `address`,
// handed to `done` on the GL thread once the frame so far has been drawn
// (empty if there is no such target).
void capture(std::uint32_t address, std::function<void(std::vector<std::uint8_t> &rgb)> done);

struct Stats {
    std::uint64_t primitives{}, batches{}, vertices{}, skipped_primitives{}, lit_vertices{};
    std::uint64_t texture_uploads{}, render_targets{}, frames{};
    std::uint64_t texture_bytes_hashed{};
    // Reading the clock is a system call on the Vita, so it is not done per
    // PRIM: list_ms covers whole command lists (decode plus draw), draw_ms
    // the GL submission inside them.
    double list_ms{}, draw_ms{}, present_ms{}, idle_ms{};
};
[[nodiscard]] const Stats &stats() noexcept;
// Time the caller spent on a command list / waiting for work (for stats()).
void account(double list_ms, double idle_ms) noexcept;

} // namespace burnout::gl
