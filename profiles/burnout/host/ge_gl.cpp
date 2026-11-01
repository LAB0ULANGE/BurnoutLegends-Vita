#include "ge_gl.hpp"

#include "gl_api.hpp"

#include "burnout_bench.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#endif

namespace burnout::gl {

volatile std::uint32_t phase{};
volatile std::uint32_t emulation_phase{};

namespace {

// ---------------------------------------------------------------- shaders

const char *kVertexShader = R"(
attribute vec3 a_position;   // PSP formats as stored, scaled here
attribute vec2 a_texcoord;
attribute vec4 a_color;      // 8888 normalised, or the raw 16-bit value in x
attribute vec3 a_normal;
uniform mat4 u_world;
uniform mat4 u_world_view;
uniform mat4 u_projection;
uniform vec4 u_viewport;   // scale x, scale y, centre x - offset x, centre y - offset y
uniform vec4 u_depth;      // scale z, centre z, through (1/0), fog enabled (1/0)
uniform vec4 u_target;     // render target width, height
uniform vec4 u_fog;        // PSP fog end, fog scale
uniform vec4 u_format;     // position, texcoord, normal scales; colour format (0 constant, 4-6 16-bit, 7 8888)
uniform vec4 u_texgen;     // mode (0 scale/offset, 1 matrix, 2 shade map, 3 through), source, unsigned z, lit
uniform vec4 u_uvscale;    // u/v scale and offset (through: 1/width, 1/height)
uniform mat4 u_texmatrix;
uniform vec4 u_shade[2];   // shade map light directions
uniform vec4 u_lighting[30];
varying vec3 v_texcoord;
varying vec4 v_color;
varying float v_fog;

vec4 vertex_color() {
    float f = u_format.w;
    if (f < 3.5 || f > 6.5) return a_color;
    float v = a_color.x;
    if (f < 4.5) return vec4(mod(v, 32.0) / 31.0, mod(floor(v / 32.0), 64.0) / 63.0, floor(v / 2048.0) / 31.0, 1.0);
    if (f < 5.5)
        return vec4(mod(v, 32.0) / 31.0, mod(floor(v / 32.0), 32.0) / 31.0, mod(floor(v / 1024.0), 32.0) / 31.0,
                    floor(v / 32768.0));
    return vec4(mod(v, 16.0), mod(floor(v / 16.0), 16.0), mod(floor(v / 256.0), 16.0), floor(v / 4096.0)) / 15.0;
}

vec3 unit(vec3 v) {
    float l = length(v);
    return l > 0.0 ? v / l : vec3(0.0, 0.0, 1.0);
}

float power(float x, float y) {
    return x > 0.0 ? pow(x, y) : (y == 0.0 ? 1.0 : 0.0);
}

// PSP vertex lighting; u_lighting: [0] material from vertex colour (x ambient,
// y diffuse, z specular), w specular power; [1] emissive; [2] global ambient;
// [3-5] material ambient, diffuse, specular; then six per light: vector and
// type (w < 0: off), attenuation and computation, spot direction and cutoff,
// ambient and spot exponent, diffuse, specular.
vec4 light(vec4 vertex, vec3 position, vec3 normal) {
    vec4 info = u_lighting[0];
    vec4 ambient = info.x > 0.5 ? vertex : u_lighting[3];
    vec4 diffuse = info.y > 0.5 ? vertex : u_lighting[4];
    vec4 specular = info.z > 0.5 ? vertex : u_lighting[5];
    vec4 result = vec4(u_lighting[1].rgb + ambient.rgb * u_lighting[2].rgb, ambient.a * u_lighting[2].a);
    for (int l = 0; l < 4; ++l) {
        vec4 l0 = u_lighting[6 + l * 6];
        if (l0.w > -0.5) {
            vec4 l1 = u_lighting[7 + l * 6];
            vec4 l2 = u_lighting[8 + l * 6];
            vec4 l3 = u_lighting[9 + l * 6];
            vec3 to_light = l0.xyz;
            float factor = 1.0;
            if (l0.w > 0.5) {
                to_light -= position;
                float distance = length(to_light);
                float d = l1.x + l1.y * distance + l1.z * distance * distance;
                factor = d > 0.0 ? clamp(1.0 / d, 0.0, 1.0) : 0.0;
                to_light = unit(to_light);
            }
            if (l0.w > 1.5) {
                float spot = dot(l2.xyz, to_light);
                factor *= spot >= l2.w ? max(0.0, power(max(spot, 0.0), l3.w)) : 0.0;
            }
            result.rgb += l3.rgb * ambient.rgb * factor;
            float lambert = dot(to_light, normal);
            if (l1.w > 1.5) lambert = power(max(lambert, 0.0), info.w);
            if (lambert > 0.0) result.rgb += u_lighting[10 + l * 6].rgb * diffuse.rgb * factor * lambert;
            if (l1.w > 0.5 && l1.w < 1.5 && lambert >= 0.0) {
                vec3 half_vector = unit(to_light + vec3(0.0, 0.0, 1.0));
                float sp = power(max(dot(half_vector, normal), 0.0), info.w);
                result.rgb += u_lighting[11 + l * 6].rgb * specular.rgb * factor * sp;
            }
        }
    }
    return clamp(result, 0.0, 1.0);
}

void main() {
    vec3 position = a_position * u_format.x;
    vec3 normal = a_normal * u_format.z;
    vec2 texcoord = a_texcoord * u_format.y;
    vec4 color = vertex_color();
    vec3 screen;
    float w = 1.0;
    v_fog = 1.0;
    if (u_depth.z > 0.5) {
        if (u_texgen.z > 0.5 && position.z < 0.0) position.z += 65536.0;
        screen = position;
    } else {
        vec4 view = u_world_view * vec4(position, 1.0);
        vec4 clip = u_projection * view;
        w = clip.w;
        vec3 ndc = clip.xyz / clip.w;
        screen = vec3(ndc.xy * u_viewport.xy + u_viewport.zw, ndc.z * u_depth.x + u_depth.y);
        if (u_depth.w > 0.5) v_fog = clamp((view.z + u_fog.x) * u_fog.y, 0.0, 1.0);
    }
    gl_Position = vec4((screen.x / u_target.x * 2.0 - 1.0) * w, (1.0 - screen.y / u_target.y * 2.0) * w,
                       (screen.z / 65535.0 * 2.0 - 1.0) * w, w);
    if (u_texgen.w > 0.5)
        color = light(color, (u_world * vec4(position, 1.0)).xyz, unit((u_world * vec4(normal, 0.0)).xyz));
    v_color = color;
    float mode = u_texgen.x;
    if (mode > 2.5) {
        v_texcoord = vec3(texcoord * u_uvscale.xy, 1.0);
    } else if (mode > 1.5) {
        vec3 n = unit((u_world * vec4(normal, 0.0)).xyz);
        v_texcoord = vec3((1.0 + dot(n, u_shade[0].xyz)) * 0.5, (1.0 + dot(n, u_shade[1].xyz)) * 0.5, 1.0);
    } else if (mode > 0.5) {
        float source = u_texgen.y;
        vec3 s = position;
        if (source > 2.5) s = normal;
        else if (source > 1.5) s = unit(normal);
        else if (source > 0.5) s = vec3(texcoord, 0.0);
        v_texcoord = (u_texmatrix * vec4(s, 1.0)).xyz;
    } else {
        v_texcoord = vec3(texcoord * u_uvscale.xy + u_uvscale.zw, 1.0);
    }
}
)";

const char *kFragmentShader = R"(
uniform sampler2D u_texture;
uniform vec4 u_texmap;     // texcoord scale u, v, offset u, v
uniform vec4 u_texenv;     // textured, function, use texture alpha, colour doubling
uniform vec4 u_envcolor;
uniform vec4 u_alpha;      // alpha test function, reference, enabled
uniform vec4 u_fogcolor;
uniform vec4 u_blendfix;   // rgb scale for a fixed source factor, w: enabled
uniform vec4 u_alphaout;   // x: replace alpha with y
varying vec3 v_texcoord;
varying vec4 v_color;
varying float v_fog;
void main() {
    vec4 c = v_color;
    if (u_texenv.x > 0.5) {
        vec2 uv = v_texcoord.xy / v_texcoord.z * u_texmap.xy + u_texmap.zw;
        vec4 t = texture2D(u_texture, uv);
        float f = u_texenv.y;
        bool tcc = u_texenv.z > 0.5;
        if (f < 0.5) {
            c = vec4(c.rgb * t.rgb, tcc ? c.a * t.a : c.a);
        } else if (f < 1.5) {
            c = vec4(tcc ? mix(c.rgb, t.rgb, t.a) : t.rgb, c.a);
        } else if (f < 2.5) {
            c = vec4(mix(c.rgb, u_envcolor.rgb, t.rgb), tcc ? c.a * t.a : c.a);
        } else if (f < 3.5) {
            c = vec4(t.rgb, tcc ? t.a : c.a);
        } else {
            c = vec4(min(c.rgb + t.rgb, 1.0), tcc ? c.a * t.a : c.a);
        }
        if (u_texenv.w > 0.5) c.rgb = min(c.rgb * 2.0, 1.0);
    }
    c.rgb = mix(u_fogcolor.rgb, c.rgb, v_fog);
    if (u_alpha.z > 0.5) {
        float a = floor(c.a * 255.0 + 0.5);
        float r = u_alpha.y;
        float func = u_alpha.x;
        bool keep = true;
        if (func < 0.5) keep = false;
        else if (func < 1.5) keep = true;
        else if (func < 2.5) keep = a == r;
        else if (func < 3.5) keep = a != r;
        else if (func < 4.5) keep = a < r;
        else if (func < 5.5) keep = a <= r;
        else if (func < 6.5) keep = a > r;
        else keep = a >= r;
        if (!keep) discard;
    }
    if (u_blendfix.w > 0.5) c.rgb *= u_blendfix.rgb;
    if (u_alphaout.x > 0.5) c.a = u_alphaout.y;
    gl_FragColor = c;
}
)";

const char *kPresentVertexShader = R"(
attribute vec2 a_position;
attribute vec2 a_texcoord;
varying vec2 v_texcoord;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)";

const char *kPresentFragmentShader = R"(
uniform sampler2D u_texture;
varying vec2 v_texcoord;
void main() {
    gl_FragColor = vec4(texture2D(u_texture, v_texcoord).rgb, 1.0);
}
)";

// ---------------------------------------------------------------- state

struct RenderTarget {
    GLuint framebuffer{}, texture{}, depth{};
    int width{}, height{};
    GLint sampling[4]{-1, -1, -1, -1};
};

struct Texture {
    GLuint name{};
    std::uint64_t hash{};
    std::uint64_t checked_frame{UINT64_MAX};
    std::uint32_t address{}, bytes{};
    bool stale{true};  // VRAM texture: a block transfer may have rewritten it
    GLint sampling[4]{-1, -1, -1, -1};
};

struct Program {
    GLuint id{};
    GLint world{}, world_view{}, projection{}, viewport{}, depth{}, target{}, fog{};
    GLint format{}, texgen{}, uvscale{}, texmatrix{}, shade{}, lighting{};
    GLint texmap{}, texenv{}, envcolor{}, alpha{}, fogcolor{}, texture{}, blendfix{}, alphaout{};
};

std::array<std::uint32_t, 256> g_commands{};
std::array<float, 12> g_world{}, g_view{}, g_texgen{};
std::array<float, 16> g_projection{};
std::uint32_t g_world_cursor{}, g_view_cursor{}, g_projection_cursor{}, g_texgen_cursor{};
std::uint32_t g_vertex_address{}, g_index_address{};
std::array<std::uint8_t, 1024> g_clut{};   // palette cached by LOAD_CLUT, as on the GE
std::uint64_t g_clut_hash{};
// Pending batch: vertices in the PSP component formats (see BatchLayout),
// all of one vertex type.
std::vector<std::uint8_t> g_batch;
std::uint32_t g_batch_count{}, g_batch_vtype{};
// Non-null: the batch vertices are read in place from the snapshot instead of
// g_batch (PSP layout already fits, see BatchLayout).
const std::uint8_t *g_batch_in_place{};
std::vector<std::uint16_t> g_batch_indices;
bool g_lighting_dirty{true};  // a lighting or material command changed since the last upload
bool g_drawing{true};         // false while a frame is skipped
std::unordered_map<std::uint32_t, RenderTarget> g_targets;
std::unordered_map<std::uint64_t, Texture> g_textures;
Program g_program, g_present;
#if defined(__vita__)
GLuint g_vertex_buffer{}, g_index_buffer{};  // wrap per-draw scratch memory (vglBufferData)
#endif
Stats g_stats;
psprecomp::GuestMemory *g_memory{};

std::uint32_t cmd(std::uint32_t index) noexcept { return g_commands[index] & 0x00FFFFFFu; }
float float24(std::uint32_t data) noexcept { return std::bit_cast<float>(data << 8u); }

GLuint compile(GLenum type, const char *source, std::string &error) {
#if defined(__vita__)
    const char *sources[1]{source};
#else
    const char *sources[2]{"#version 120\n", source};
#endif
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, static_cast<GLsizei>(std::size(sources)), sources, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        error = std::string("shader compile failed: ") + log;
        return 0u;
    }
    return shader;
}

bool link(Program &program, const char *vertex, const char *fragment, bool present, std::string &error) {
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex, error);
    if (vs == 0u) return false;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, error);
    if (fs == 0u) return false;
    program.id = glCreateProgram();
    glAttachShader(program.id, vs);
    glAttachShader(program.id, fs);
    glBindAttribLocation(program.id, 0, "a_position");
    glBindAttribLocation(program.id, 1, "a_texcoord");
    if (!present) {
        glBindAttribLocation(program.id, 2, "a_color");
        glBindAttribLocation(program.id, 3, "a_normal");
    }
    glLinkProgram(program.id);
    GLint ok = 0;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetProgramInfoLog(program.id, sizeof(log), nullptr, log);
        error = std::string("program link failed: ") + log;
        return false;
    }
    const auto u = [&](const char *name) { return glGetUniformLocation(program.id, name); };
    program.texture = u("u_texture");
    if (present) return true;
    program.world = u("u_world");
    program.world_view = u("u_world_view");
    program.format = u("u_format");
    program.texgen = u("u_texgen");
    program.uvscale = u("u_uvscale");
    program.texmatrix = u("u_texmatrix");
    program.shade = u("u_shade");
    program.lighting = u("u_lighting");
    program.projection = u("u_projection");
    program.viewport = u("u_viewport");
    program.depth = u("u_depth");
    program.target = u("u_target");
    program.fog = u("u_fog");
    program.texmap = u("u_texmap");
    program.texenv = u("u_texenv");
    program.envcolor = u("u_envcolor");
    program.alpha = u("u_alpha");
    program.fogcolor = u("u_fogcolor");
    program.blendfix = u("u_blendfix");
    program.alphaout = u("u_alphaout");
    return true;
}

// ---------------------------------------------------------------- GL thread
//
// The draw path runs on the decode thread and only records GL work into a
// Frame; the GL thread, which owns the context, replays it. Translating the
// GE commands and submitting to vitaGL then run on two cores at once.
// Textures and render targets are named by ids the decode thread assigns;
// the GL thread creates the objects on first use.

enum class Op : std::uint32_t {
    Enable, Disable, BlendFunc, BlendEquation, DepthFunc, DepthMask, ColorMask, FrontFace, Scissor, Viewport,
    UseProgram, BindFramebuffer, BindTexture, TexParameter, TexImage, Uniform4, UniformMatrix, Draw, CreateTarget,
    Present, Capture
};

// One vertex attribute of a Draw: an array in the batch, or a constant.
struct AttribSetup {
    std::int32_t size{};  // 0: constant
    GLenum type{};
    std::uint32_t normalized{}, offset{};
    float constant[4]{};
};

struct Frame {
    std::vector<std::uint32_t> ops;
    std::vector<std::uint8_t> data;                 // payloads, 8-byte aligned
    std::vector<std::vector<std::uint8_t>> blobs;   // texture images
    std::vector<Snapshot> snapshots;                // in-place vertices point into these
    std::vector<std::function<void(std::vector<std::uint8_t> &)>> captures;
};

Frame g_frame;  // being recorded by the decode thread

void record(Op op, std::initializer_list<std::uint32_t> args = {}) {
    g_frame.ops.push_back(static_cast<std::uint32_t>(op));
    g_frame.ops.insert(g_frame.ops.end(), args.begin(), args.end());
}

std::uint32_t record_data(const void *bytes, std::size_t size) {
    const std::size_t at = (g_frame.data.size() + 7u) & ~std::size_t{7u};
    g_frame.data.resize(at + size);
    if (size != 0u) std::memcpy(g_frame.data.data() + at, bytes, size);
    return static_cast<std::uint32_t>(at);
}

std::uint32_t float_bits(float value) { return std::bit_cast<std::uint32_t>(value); }

GLuint g_next_texture_id{1u}, g_next_framebuffer_id{1u};

// GL state of the draw path, so unchanged state is not sent again (vitaGL
// does work on every call). present(), capture() and render target
// creation change state behind it and call forget_state().
struct GlState {
    GLuint framebuffer{~0u}, program{~0u}, texture{~0u};
    int viewport_w{-1}, viewport_h{-1};
    int scissor[4]{-1, -1, -1, -1};
    int blend{-1}, depth_test{-1}, cull{-1}, scissor_test{-1};
    GLenum blend_src{}, blend_dst{}, blend_equation{}, depth_func{}, front_face{};
    int depth_mask{-1}, color_mask{-1};
} g_state;

void forget_state() { g_state = GlState{}; }

void set_capability(GLenum cap, int &cached, bool on) {
    if (cached == static_cast<int>(on)) return;
    cached = on ? 1 : 0;
    record(on ? Op::Enable : Op::Disable, {cap});
}

void set_blend_func(GLenum src, GLenum dst) {
    if (g_state.blend_src == src && g_state.blend_dst == dst) return;
    g_state.blend_src = src;
    g_state.blend_dst = dst;
    record(Op::BlendFunc, {src, dst});
}

void set_blend_equation(GLenum equation) {
    if (g_state.blend_equation == equation) return;
    g_state.blend_equation = equation;
    record(Op::BlendEquation, {equation});
}

void set_depth(GLenum func, bool write) {
    if (g_state.depth_func != func) {
        g_state.depth_func = func;
        record(Op::DepthFunc, {func});
    }
    if (g_state.depth_mask != static_cast<int>(write)) {
        g_state.depth_mask = write ? 1 : 0;
        record(Op::DepthMask, {write ? 1u : 0u});
    }
}

void set_color_mask(bool rgb, bool alpha) {
    const int mask = (rgb ? 1 : 0) | (alpha ? 2 : 0);
    if (g_state.color_mask == mask) return;
    g_state.color_mask = mask;
    record(Op::ColorMask, {rgb ? 1u : 0u, alpha ? 1u : 0u});
}

void bind_texture_object(GLuint name) {
    if (g_state.texture == name) return;
    g_state.texture = name;
    record(Op::BindTexture, {name});
}

// Sampler parameters live in the texture object: only set when they change.
void set_sampling(GLint (&current)[4], bool linear, bool clamp_u, bool clamp_v) {
    const GLint wanted[4]{linear ? GL_LINEAR : GL_NEAREST, linear ? GL_LINEAR : GL_NEAREST,
                          clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT, clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT};
    static constexpr GLenum names[4]{GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T};
    for (int i = 0; i < 4; ++i)
        if (current[i] != wanted[i]) {
            current[i] = wanted[i];
            record(Op::TexParameter, {names[i], static_cast<std::uint32_t>(wanted[i])});
        }
}

// ---------------------------------------------------------------- render targets

std::uint32_t target_key(std::uint32_t address) noexcept { return address & 0x001FFFF0u; }

RenderTarget &render_target(std::uint32_t key, std::uint32_t stride) {
    auto [it, inserted] = g_targets.try_emplace(key);
    RenderTarget &target = it->second;
    if (!inserted) return target;
    target.width = static_cast<int>(std::max<std::uint32_t>(stride, 64u));
    target.height = 272;
    target.texture = g_next_texture_id++;
    target.framebuffer = g_next_framebuffer_id++;
    record(Op::CreateTarget, {target.framebuffer, target.texture, static_cast<std::uint32_t>(target.width),
                              static_cast<std::uint32_t>(target.height)});
    // The GL thread binds and clears it, and sets its sampling.
    forget_state();
    for (GLint &value : target.sampling) value = -1;
    target.sampling[0] = target.sampling[1] = GL_LINEAR;
    target.sampling[2] = target.sampling[3] = GL_CLAMP_TO_EDGE;
    ++g_stats.render_targets;
    return target;
}

// ---------------------------------------------------------------- textures

std::uint32_t texture_bits(std::uint32_t format) noexcept {
    switch (format) {
    case 3u: case 7u: return 32u;
    case 4u: return 4u;
    case 5u: return 8u;
    case 8u: return 4u;
    case 9u: case 10u: return 8u;
    default: return 16u;
    }
}

std::array<std::uint8_t, 4> color16(std::uint16_t p, std::uint32_t format) noexcept {
    const auto e5 = [](std::uint32_t v) { return static_cast<std::uint8_t>((v << 3u) | (v >> 2u)); };
    const auto e6 = [](std::uint32_t v) { return static_cast<std::uint8_t>((v << 2u) | (v >> 4u)); };
    const auto e4 = [](std::uint32_t v) { return static_cast<std::uint8_t>(v * 17u); };
    switch (format) {
    case 0u: return {e5(p & 31u), e6((p >> 5u) & 63u), e5((p >> 11u) & 31u), 255u};
    case 1u: return {e5(p & 31u), e5((p >> 5u) & 31u), e5((p >> 10u) & 31u), static_cast<std::uint8_t>((p >> 15u) ? 255u : 0u)};
    default: return {e4(p & 15u), e4((p >> 4u) & 15u), e4((p >> 8u) & 15u), e4((p >> 12u) & 15u)};
    }
}

std::array<std::uint8_t, 4> clut_color(std::uint32_t index) noexcept {
    const std::uint32_t clut_format = cmd(0xC5u);
    const std::uint32_t shift = (clut_format >> 2u) & 31u;
    const std::uint32_t mask = (clut_format >> 8u) & 0xFFu;
    const std::uint32_t start = ((clut_format >> 16u) & 31u) << 4u;
    const std::uint32_t entry = (((index >> shift) & mask) | start) & 0xFFu;
    if ((clut_format & 3u) == 3u) {
        const std::uint8_t *p = &g_clut[(entry * 4u) & 1023u];
        return {p[0], p[1], p[2], p[3]};
    }
    const std::uint8_t *p = &g_clut[(entry * 2u) & 1023u];
    return color16(static_cast<std::uint16_t>(p[0] | (p[1] << 8u)), clut_format & 3u);
}

std::uint64_t fnv(const std::uint8_t *data, std::size_t size, std::uint64_t hash = 0xCBF29CE484222325ull) noexcept {
    std::size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        std::uint64_t word;
        std::memcpy(&word, data + i, 8u);
        hash = (hash ^ word) * 0x100000001B3ull;
    }
    for (; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001B3ull;
    return hash;
}

// Binds the texture for the current state and returns the texcoord transform
// (scale u, v, offset u, v) that maps PSP normalized coordinates to it.
std::array<float, 4> bind_texture(psprecomp::GuestMemory &memory) {
    const std::uint32_t address = (cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u);
    const std::uint32_t buffer_width = cmd(0xA8u) & 0x7FFu;
    const std::uint32_t width = 1u << std::min<std::uint32_t>(cmd(0xB8u) & 0xFu, 10u);
    const std::uint32_t height = 1u << std::min<std::uint32_t>((cmd(0xB8u) >> 8u) & 0xFu, 10u);
    const std::uint32_t format = cmd(0xC3u) & 0xFu;
    const bool swizzled = (cmd(0xC2u) & 1u) != 0u;
    const bool clamp_u = (cmd(0xC7u) & 1u) != 0u, clamp_v = (cmd(0xC7u) & 0x100u) != 0u;
    const bool linear = (cmd(0xC6u) & 0x100u) != 0u;

    // Render to texture: sample the framebuffer object, flipped vertically.
    if ((address & 0x0F000000u) == 0x04000000u) {
        const auto it = g_targets.find(target_key(address));
        if (it != g_targets.end()) {
            bind_texture_object(it->second.texture);
            set_sampling(it->second.sampling, linear, true, true);
            return {static_cast<float>(width) / static_cast<float>(it->second.width),
                    -static_cast<float>(height) / static_cast<float>(it->second.height), 0.0f, 1.0f};
        }
    }

    const std::uint32_t bits = texture_bits(format);
    const std::uint32_t row_bytes = std::max<std::uint32_t>(buffer_width, width) * bits / 8u;
    const std::uint32_t bytes = row_bytes * height;
    const std::uint8_t *source = memory.raw_pointer(address, bytes);
    const bool paletted = format >= 4u && format <= 7u;
    std::uint64_t key = (static_cast<std::uint64_t>(address) << 32u) ^ (static_cast<std::uint64_t>(format) << 28u) ^
                        (static_cast<std::uint64_t>(width) << 16u) ^ (static_cast<std::uint64_t>(height) << 4u) ^
                        (swizzled ? 1u : 0u) ^ (static_cast<std::uint64_t>(buffer_width) << 40u);
    if (paletted) key ^= (g_clut_hash ^ cmd(0xC5u)) * 31u;
    Texture &texture = g_textures[key];
    const bool fresh = texture.name == 0u;
    if (fresh) {
        texture.name = g_next_texture_id++;
        texture.address = address;
        texture.bytes = bytes;
    }
    bind_texture_object(texture.name);
    // Only block transfers write VRAM (see invalidate_textures); textures in
    // RAM are re-hashed once per frame.
    const bool in_vram = (address & 0x0F000000u) == 0x04000000u;
    if ((in_vram ? texture.stale : texture.checked_frame != g_stats.frames) && source != nullptr) {
        texture.checked_frame = g_stats.frames;
        texture.stale = false;
        const std::uint64_t hash = fnv(source, bytes);
        if (fresh || hash != texture.hash) {
            texture.hash = hash;
            std::vector<std::uint8_t> linear_bytes;
            const std::uint8_t *data = source;
            if (swizzled) {
                // 16-byte x 8-row blocks, row-major.
                linear_bytes.resize(bytes);
                const std::uint32_t blocks_per_row = std::max<std::uint32_t>(row_bytes / 16u, 1u);
                for (std::uint32_t y = 0; y < height; ++y)
                    for (std::uint32_t x = 0; x + 16u <= row_bytes; x += 16u) {
                        const std::uint32_t block = (y / 8u) * blocks_per_row + x / 16u;
                        std::memcpy(&linear_bytes[y * row_bytes + x], source + block * 128u + (y % 8u) * 16u, 16u);
                    }
                data = linear_bytes.data();
            }
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u);
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint8_t *row = data + y * row_bytes;
                std::uint8_t *out = &rgba[static_cast<std::size_t>(y) * width * 4u];
                for (std::uint32_t x = 0; x < width; ++x, out += 4) {
                    std::array<std::uint8_t, 4> c{255u, 0u, 255u, 255u};
                    switch (format) {
                    case 0u: case 1u: case 2u: c = color16(static_cast<std::uint16_t>(row[x * 2u] | (row[x * 2u + 1u] << 8u)), format); break;
                    case 3u: c = {row[x * 4u], row[x * 4u + 1u], row[x * 4u + 2u], row[x * 4u + 3u]}; break;
                    case 4u: c = clut_color((row[x / 2u] >> ((x & 1u) * 4u)) & 15u); break;
                    case 5u: c = clut_color(row[x]); break;
                    case 6u: c = clut_color(row[x * 2u] | (row[x * 2u + 1u] << 8u)); break;
                    case 7u: c = clut_color(row[x * 4u]); break;
                    default: break;  // DXT: not used by this game so far
                    }
                    std::memcpy(out, c.data(), 4u);
                }
            }
            record(Op::TexImage, {width, height, static_cast<std::uint32_t>(g_frame.blobs.size())});
            g_frame.blobs.push_back(std::move(rgba));
            ++g_stats.texture_uploads;
        }
    }
    set_sampling(texture.sampling, linear, clamp_u, clamp_v);
    return {1.0f, 1.0f, 0.0f, 0.0f};
}

// ---------------------------------------------------------------- GL state

GLenum compare(std::uint32_t function) noexcept {
    static constexpr GLenum table[8]{GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return table[function & 7u];
}

// GXM has no constant blend colour, so FIXA/FIXB are emulated the way PPSSPP
// does: a fixed source factor scales the shader output (blend factor ONE), and
// a fixed destination factor becomes ONE_MINUS_SRC_ALPHA with the shader
// writing alpha = 1 - FIXB (grey FIXB only).
struct BlendShader {
    float scale[3]{1.0f, 1.0f, 1.0f};
    bool scaled{};
    float alpha{-1.0f};  // >= 0: written as the output alpha
};

BlendShader set_blend() {
    BlendShader shader;
    if ((cmd(0x21u) & 1u) == 0u || (cmd(0xD3u) & 1u) != 0u) {
        set_capability(GL_BLEND, g_state.blend, false);
        return shader;
    }
    set_capability(GL_BLEND, g_state.blend, true);
    const std::uint32_t blend = cmd(0xDFu);
    const std::uint32_t fix_a = cmd(0xE0u), fix_b = cmd(0xE1u);
    static constexpr GLenum source_table[10]{GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                             GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                             GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    static constexpr GLenum destination_table[10]{GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    const std::uint32_t s = blend & 0xFu, d = (blend >> 4u) & 0xFu;
    GLenum source = s < 10u ? source_table[s] : GL_ONE;
    if (s >= 10u) {
        if (fix_a == 0u) source = GL_ZERO;
        else if (fix_a != 0xFFFFFFu) {
            shader.scaled = true;
            for (int i = 0; i < 3; ++i) shader.scale[i] = static_cast<float>((fix_a >> (i * 8)) & 0xFFu) / 255.0f;
        }
    }
    GLenum destination = d < 10u ? destination_table[d] : GL_ONE;
    if (d >= 10u) {
        if (fix_b == 0u) destination = GL_ZERO;
        else if (fix_b != 0xFFFFFFu && source != GL_SRC_ALPHA && source != GL_ONE_MINUS_SRC_ALPHA) {
            const float grey = static_cast<float>((fix_b & 0xFFu) + ((fix_b >> 8u) & 0xFFu) + ((fix_b >> 16u) & 0xFFu)) / 765.0f;
            destination = GL_ONE_MINUS_SRC_ALPHA;
            shader.alpha = 1.0f - grey;
        }
    }
    set_blend_func(source, destination);
    static constexpr GLenum equations[6]{GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD};
    set_blend_equation(equations[std::min<std::uint32_t>((blend >> 8u) & 7u, 5u)]);
    return shader;
}

// Every glUniform makes vitaGL rebuild the program's uniform buffer, so values
// that did not change since the previous draw are not sent again. A short
// flat table: the draw program has about twenty uniforms.
struct CachedUniform {
    GLint location{-1};
    std::uint32_t count{};
    std::array<float, 30 * 4> values{};
};
std::array<CachedUniform, 32> g_uniforms;
std::size_t g_uniform_count{};

bool uniform_changed(GLint location, const float *values, std::size_t count) {
    if (location < 0) return false;
    CachedUniform *cached = nullptr;
    for (std::size_t i = 0; i < g_uniform_count; ++i)
        if (g_uniforms[i].location == location) { cached = &g_uniforms[i]; break; }
    if (cached == nullptr) {
        if (g_uniform_count == g_uniforms.size() || count > g_uniforms[0].values.size()) return true;
        cached = &g_uniforms[g_uniform_count++];
        cached->location = location;
    } else if (cached->count == count && std::memcmp(cached->values.data(), values, count * sizeof(float)) == 0) {
        return false;
    }
    cached->count = static_cast<std::uint32_t>(count);
    std::memcpy(cached->values.data(), values, count * sizeof(float));
    return true;
}


void uniform4(GLint location, float x, float y, float z, float w) {
    const float values[4]{x, y, z, w};
    if (uniform_changed(location, values, 4u))
        record(Op::Uniform4, {static_cast<std::uint32_t>(location), 1u, record_data(values, sizeof(values))});
}

void uniform4v(GLint location, const float *values, int count) {
    if (uniform_changed(location, values, static_cast<std::size_t>(count) * 4u))
        record(Op::Uniform4, {static_cast<std::uint32_t>(location), static_cast<std::uint32_t>(count),
                              record_data(values, static_cast<std::size_t>(count) * 4u * sizeof(float))});
}

void uniform_matrix(GLint location, const float *values) {
    if (uniform_changed(location, values, 16u))
        record(Op::UniformMatrix, {static_cast<std::uint32_t>(location), record_data(values, 16u * sizeof(float))});
}

// PSP 4x3 row-vector matrix (v * M) as a GL matrix: stored row-major, it is
// exactly the column-major M^T.
void psp_matrix(const float *m, float *out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = m[r * 3 + c];
    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
}

void upload_matrices() {
    // PSP 4x3 matrices are row-vector (v * M); GL wants column-major M^T.
    float world[16]{}, view[16]{}, world_view[16]{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            world[r * 4 + c] = g_world[r * 3 + c];
            view[r * 4 + c] = g_view[r * 3 + c];
        }
    world[15] = view[15] = 1.0f;
    // Row-vector convention: v * world * view.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += world[r * 4 + k] * view[k * 4 + c];
            world_view[r * 4 + c] = sum;
        }
    // A row-vector matrix stored row-major is exactly a column-major GL matrix.
    uniform_matrix(g_program.world, world);
    uniform_matrix(g_program.world_view, world_view);
    uniform_matrix(g_program.projection, g_projection.data());
}

void upload_vertex_state(bool through);
void bind_vertices();

void flush() {
    if (g_batch_indices.empty() || !g_drawing) {
        g_batch_indices.clear();
        g_batch.clear();
        g_batch_count = 0u;
        g_batch_in_place = nullptr;
        return;
    }
    const std::uint32_t outer_phase = phase;
    phase = 6u;
    const double started = host_now_ms();
    const std::uint32_t address = cmd(0x9Cu);
    const std::uint32_t stride = cmd(0x9Du) & 0x7FCu;
    RenderTarget &target = render_target(target_key(address), stride);
    if (g_state.framebuffer != target.framebuffer) {
        g_state.framebuffer = target.framebuffer;
        record(Op::BindFramebuffer, {target.framebuffer});
    }
    if (g_state.viewport_w != target.width || g_state.viewport_h != target.height) {
        g_state.viewport_w = target.width;
        g_state.viewport_h = target.height;
        record(Op::Viewport, {static_cast<std::uint32_t>(target.width), static_cast<std::uint32_t>(target.height)});
    }
    if (g_state.program != g_program.id) {
        g_state.program = g_program.id;
        record(Op::UseProgram, {g_program.id});
    }

    const bool clear_mode = (cmd(0xD3u) & 1u) != 0u;
    const bool through = (g_batch_vtype & 0x800000u) != 0u;
    if (!through) upload_matrices();
    upload_vertex_state(through);
    uniform4(g_program.viewport, float24(cmd(0x42u)), float24(cmd(0x43u)),
             float24(cmd(0x45u)) - static_cast<float>(cmd(0x4Cu) & 0xFFFFu) / 16.0f,
             float24(cmd(0x46u)) - static_cast<float>(cmd(0x4Du) & 0xFFFFu) / 16.0f);
    const bool fog = !through && !clear_mode && (cmd(0x1Fu) & 1u) != 0u;
    uniform4(g_program.depth, float24(cmd(0x44u)), float24(cmd(0x47u)), through ? 1.0f : 0.0f, fog ? 1.0f : 0.0f);
    uniform4(g_program.target, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 0.0f);
    uniform4(g_program.fog, float24(cmd(0xCDu)), float24(cmd(0xCEu)), 0.0f, 0.0f);
    const std::uint32_t fog_color = cmd(0xCFu);
    uniform4(g_program.fogcolor, static_cast<float>(fog_color & 0xFFu) / 255.0f,
                static_cast<float>((fog_color >> 8u) & 0xFFu) / 255.0f, static_cast<float>((fog_color >> 16u) & 0xFFu) / 255.0f, 1.0f);

    const bool textured = !clear_mode && (cmd(0x1Eu) & 1u) != 0u;
    if (textured) {
        phase = 7u;
        const auto texmap = bind_texture(*g_memory);
        phase = 8u;
        uniform4(g_program.texmap, texmap[0], texmap[1], texmap[2], texmap[3]);
    }
    const std::uint32_t texfunc = cmd(0xC9u);
    uniform4(g_program.texenv, textured ? 1.0f : 0.0f, static_cast<float>(std::min<std::uint32_t>(texfunc & 7u, 4u)),
                (texfunc & 0x100u) != 0u ? 1.0f : 0.0f, (texfunc & 0x10000u) != 0u ? 1.0f : 0.0f);
    const std::uint32_t env = cmd(0xCAu);
    uniform4(g_program.envcolor, static_cast<float>(env & 0xFFu) / 255.0f, static_cast<float>((env >> 8u) & 0xFFu) / 255.0f,
                static_cast<float>((env >> 16u) & 0xFFu) / 255.0f, 1.0f);
    const std::uint32_t alpha_test = cmd(0xDBu);
    const bool alpha_enabled = !clear_mode && (cmd(0x22u) & 1u) != 0u;
    uniform4(g_program.alpha, static_cast<float>(alpha_test & 7u), static_cast<float>((alpha_test >> 8u) & 0xFFu),
                alpha_enabled ? 1.0f : 0.0f, 0.0f);

    // Scissor (inclusive PSP rectangle, y down) in GL window coordinates.
    const std::uint32_t s1 = cmd(0xD4u), s2 = cmd(0xD5u);
    const int x1 = static_cast<int>(s1 & 0x3FFu), y1 = static_cast<int>((s1 >> 10u) & 0x3FFu);
    const int x2 = static_cast<int>(s2 & 0x3FFu), y2 = static_cast<int>((s2 >> 10u) & 0x3FFu);
    set_capability(GL_SCISSOR_TEST, g_state.scissor_test, true);
    const int scissor[4]{x1, target.height - (y2 + 1), std::max(0, x2 - x1 + 1), std::max(0, y2 - y1 + 1)};
    if (std::memcmp(scissor, g_state.scissor, sizeof(scissor)) != 0) {
        std::memcpy(g_state.scissor, scissor, sizeof(scissor));
        record(Op::Scissor, {static_cast<std::uint32_t>(scissor[0]), static_cast<std::uint32_t>(scissor[1]),
                             static_cast<std::uint32_t>(scissor[2]), static_cast<std::uint32_t>(scissor[3])});
    }

    if (clear_mode) {
        const std::uint32_t clear = cmd(0xD3u);
        set_capability(GL_BLEND, g_state.blend, false);
        set_capability(GL_DEPTH_TEST, g_state.depth_test, true);
        set_depth(GL_ALWAYS, (clear & 0x400u) != 0u);
        set_color_mask((clear & 0x100u) != 0u, (clear & 0x200u) != 0u);
        set_capability(GL_CULL_FACE, g_state.cull, false);
        uniform4(g_program.blendfix, 1.0f, 1.0f, 1.0f, 0.0f);
        uniform4(g_program.alphaout, 0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        const BlendShader blend = set_blend();
        uniform4(g_program.blendfix, blend.scale[0], blend.scale[1], blend.scale[2], blend.scaled ? 1.0f : 0.0f);
        uniform4(g_program.alphaout, blend.alpha >= 0.0f ? 1.0f : 0.0f, std::max(blend.alpha, 0.0f), 0.0f, 0.0f);
        // Depth writes still happen with the test off.
        set_capability(GL_DEPTH_TEST, g_state.depth_test, true);
        set_depth((cmd(0x23u) & 1u) != 0u ? compare(cmd(0xDEu)) : GL_ALWAYS, (cmd(0xE7u) & 1u) == 0u);
        set_color_mask(true, true);
        static const bool no_cull = std::getenv("PSPRECOMP_BURNOUT_NOCULL") != nullptr;
        const bool cull = !no_cull && !through && (cmd(0x1Du) & 1u) != 0u;
        set_capability(GL_CULL_FACE, g_state.cull, cull);
        if (cull) {
            const GLenum front = (cmd(0x9Bu) & 1u) != 0u ? GL_CW : GL_CCW;  // y is flipped on the way to GL
            if (g_state.front_face != front) {
                g_state.front_face = front;
                record(Op::FrontFace, {front});
            }
        }
    }

    phase = 9u;
    bind_vertices();
    ++g_stats.batches;
    g_stats.vertices += g_batch_count;
    g_batch.clear();
    g_batch_count = 0u;
    g_batch_in_place = nullptr;
    g_batch_indices.clear();
    g_stats.draw_ms += host_now_ms() - started;
    phase = outer_phase;
}

// ---------------------------------------------------------------- vertices

std::array<float, 3> normalized(std::array<float, 3> v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 0.0f)) return {0.0f, 0.0f, 1.0f};
    return {v[0] / length, v[1] / length, v[2] / length};
}

std::array<float, 4> rgb_command(std::uint32_t index, float alpha) {
    const std::uint32_t v = cmd(index);
    return {static_cast<float>(v & 0xFFu) / 255.0f, static_cast<float>((v >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>((v >> 16u) & 0xFFu) / 255.0f, alpha};
}

// PSP vertex lighting state for the vertex shader (layout in kVertexShader).
std::array<float, 30 * 4> lighting_uniforms(bool has_color) {
    std::array<float, 30 * 4> u{};
    const auto put = [&](int slot, std::array<float, 4> v) { std::copy(v.begin(), v.end(), u.begin() + slot * 4); };
    const std::uint32_t update = cmd(0x53u) & (has_color ? 7u : 0u);
    float exponent = float24(cmd(0x5Bu));
    if (!std::isfinite(exponent) || exponent < 0.0f) exponent = 0.0f;
    put(0, {(update & 1u) ? 1.0f : 0.0f, (update & 2u) ? 1.0f : 0.0f, (update & 4u) ? 1.0f : 0.0f, exponent});
    put(1, rgb_command(0x54u, 0.0f));
    put(2, rgb_command(0x5Cu, static_cast<float>(cmd(0x5Du) & 0xFFu) / 255.0f));
    const float material_alpha = static_cast<float>(cmd(0x58u) & 0xFFu) / 255.0f;
    put(3, rgb_command(0x55u, material_alpha));
    put(4, rgb_command(0x56u, material_alpha));
    put(5, rgb_command(0x57u, material_alpha));
    for (std::uint32_t l = 0; l < 4u; ++l) {
        const int base = 6 + static_cast<int>(l) * 6;
        if ((cmd(0x18u + l) & 1u) == 0u) {
            put(base, {0.0f, 0.0f, 0.0f, -1.0f});
            continue;
        }
        const std::uint32_t type_data = cmd(0x5Fu + l);
        const std::uint32_t type = (type_data >> 8u) & 3u;
        std::array<float, 3> vector{float24(cmd(0x63u + l * 3u)), float24(cmd(0x64u + l * 3u)), float24(cmd(0x65u + l * 3u))};
        if (type == 0u) vector = normalized(vector);
        put(base, {vector[0], vector[1], vector[2], static_cast<float>(type)});
        put(base + 1, {float24(cmd(0x7Bu + l * 3u)), float24(cmd(0x7Cu + l * 3u)), float24(cmd(0x7Du + l * 3u)),
                       static_cast<float>(type_data & 3u)});
        const auto direction =
            normalized({float24(cmd(0x6Fu + l * 3u)), float24(cmd(0x70u + l * 3u)), float24(cmd(0x71u + l * 3u))});
        put(base + 2, {direction[0], direction[1], direction[2], float24(cmd(0x8Bu + l))});
        auto ambient = rgb_command(0x8Fu + l * 3u, 1.0f);
        ambient[3] = float24(cmd(0x87u + l));
        put(base + 3, ambient);
        put(base + 4, rgb_command(0x90u + l * 3u, 1.0f));
        put(base + 5, rgb_command(0x91u + l * 3u, 1.0f));
    }
    return u;
}

// Vertex layout: weights, texcoord, colour, normal, position; each aligned to its size.
constexpr std::uint32_t sizes_tc[4]{0, 1, 2, 4}, sizes_col[8]{0, 0, 0, 0, 2, 2, 2, 4};
constexpr std::uint32_t sizes_nrm[4]{0, 1, 2, 4}, sizes_pos[4]{0, 1, 2, 4}, sizes_w[4]{0, 1, 2, 4};

struct VertexLayout {
    std::uint32_t tc{}, col{}, nrm{}, pos{}, stride{};
};

VertexLayout vertex_layout(std::uint32_t vtype) {
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const std::uint32_t weight = (vtype >> 9u) & 3u;
    const std::uint32_t weights = weight != 0u ? ((vtype >> 14u) & 7u) + 1u : 0u;
    std::uint32_t offset = 0u, alignment = 1u;
    const auto place = [&](std::uint32_t size, std::uint32_t n) {
        offset = (offset + size - 1u) & ~(size - 1u);
        const std::uint32_t at = offset;
        offset += size * n;
        alignment = std::max(alignment, size);
        return at;
    };
    VertexLayout layout;
    if (weights != 0u) place(sizes_w[weight], weights);
    layout.tc = tc ? place(sizes_tc[tc], 2u) : 0u;
    layout.col = col >= 4u ? place(sizes_col[col], 1u) : 0u;
    layout.nrm = nrm ? place(sizes_nrm[nrm], 3u) : 0u;
    layout.pos = place(std::max<std::uint32_t>(sizes_pos[pos], 1u), 3u);
    layout.stride = (offset + alignment - 1u) & ~(alignment - 1u);
    return layout;
}

// Batch vertices keep the PSP component formats, which the vertex shader
// scales and converts; each field sits in a 4-byte aligned slot, as GXM
// vertex streams want. Common types already have this layout and are copied
// as one block.
struct BatchLayout {
    std::uint32_t tc{}, col{}, nrm{}, pos{}, stride{};
    std::uint32_t tc_size{}, col_size{}, nrm_size{}, pos_size{};  // bytes per component (colour: whole value)
    bool same_as_psp{};
};

BatchLayout batch_layout(std::uint32_t vtype) {
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const auto align4 = [](std::uint32_t v) { return (v + 3u) & ~3u; };
    BatchLayout layout;
    layout.tc_size = sizes_tc[tc];
    layout.col_size = sizes_col[col];
    layout.nrm_size = sizes_nrm[nrm];
    layout.pos_size = std::max<std::uint32_t>(sizes_pos[pos], 1u);
    std::uint32_t offset = 0u;
    layout.tc = offset;
    offset += align4(layout.tc_size * 2u);
    layout.col = offset;
    offset += align4(layout.col_size);
    layout.nrm = offset;
    offset += align4(layout.nrm_size * 3u);
    layout.pos = offset;
    offset += align4(layout.pos_size * 3u);
    layout.stride = offset;
    const VertexLayout psp = vertex_layout(vtype);
    if (psp.stride % 4u == 0u) {
        // The PSP aligns each field to its component size; with a 4-byte
        // multiple stride that layout is used as it is.
        layout.tc = psp.tc;
        layout.col = psp.col;
        layout.nrm = psp.nrm;
        layout.pos = psp.pos;
        layout.stride = psp.stride;
    }
    layout.same_as_psp = (tc == 0u || psp.tc == layout.tc) && (layout.col_size == 0u || psp.col == layout.col) &&
                         (nrm == 0u || psp.nrm == layout.nrm) && psp.pos == layout.pos && psp.stride == layout.stride;
    return layout;
}

void copy_vertex(std::uint8_t *out, const std::uint8_t *in, const BatchLayout &to, const VertexLayout &from) {
    std::memcpy(out + to.tc, in + from.tc, to.tc_size * 2u);
    std::memcpy(out + to.col, in + from.col, to.col_size);
    std::memcpy(out + to.nrm, in + from.nrm, to.nrm_size * 3u);
    std::memcpy(out + to.pos, in + from.pos, to.pos_size * 3u);
}

BatchLayout g_layout;

std::uint8_t *append_vertices(std::uint32_t count) {
    const std::size_t at = g_batch.size();
    g_batch.resize(at + static_cast<std::size_t>(count) * g_layout.stride);
    g_batch_count += count;
    return g_batch.data() + at;
}

GLenum component_type(std::uint32_t format, bool is_signed) {
    switch (format) {
    case 1u: return is_signed ? GL_BYTE : GL_UNSIGNED_BYTE;
    case 2u: return is_signed ? GL_SHORT : GL_UNSIGNED_SHORT;
    default: return GL_FLOAT;
    }
}

// Vertex format, texture-coordinate generation and lighting uniforms of the
// pending batch.
void upload_vertex_state(bool through) {
    const std::uint32_t vtype = g_batch_vtype;
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const float tc_scale = through ? 1.0f : (tc == 1u ? 1.0f / 128.0f : tc == 2u ? 1.0f / 32768.0f : 1.0f);
    const float pos_scale = through ? 1.0f : (pos == 1u ? 1.0f / 128.0f : pos == 2u ? 1.0f / 32768.0f : 1.0f);
    const float nrm_scale = nrm == 1u ? 1.0f / 127.0f : nrm == 2u ? 1.0f / 32767.0f : 1.0f;
    uniform4(g_program.format, pos_scale, tc_scale, nrm_scale, static_cast<float>(col >= 4u ? col : 0u));
    const bool lit = !through && (cmd(0x17u) & 1u) != 0u;
    const std::uint32_t map_mode = cmd(0xC0u) & 3u, map_source = (cmd(0xC0u) >> 8u) & 3u;
    const float mode = through ? 3.0f : (map_mode == 1u || map_mode == 2u) ? static_cast<float>(map_mode) : 0.0f;
    uniform4(g_program.texgen, mode, static_cast<float>(map_source), through && pos == 2u ? 1.0f : 0.0f, lit ? 1.0f : 0.0f);
    if (through) {
        const float width = static_cast<float>(1u << std::min<std::uint32_t>(cmd(0xB8u) & 0xFu, 10u));
        const float height = static_cast<float>(1u << std::min<std::uint32_t>((cmd(0xB8u) >> 8u) & 0xFu, 10u));
        uniform4(g_program.uvscale, 1.0f / width, 1.0f / height, 0.0f, 0.0f);
    } else if (mode == 0.0f) {
        uniform4(g_program.uvscale, float24(cmd(0x48u)), float24(cmd(0x49u)), float24(cmd(0x4Au)), float24(cmd(0x4Bu)));
    } else if (mode == 1.0f) {
        float texmatrix[16];
        psp_matrix(g_texgen.data(), texmatrix);
        uniform_matrix(g_program.texmatrix, texmatrix);
    } else {
        const std::uint32_t ls = cmd(0xC1u);
        const auto direction = [&](std::uint32_t l) {
            return normalized({float24(cmd(0x63u + l * 3u)), float24(cmd(0x64u + l * 3u)), float24(cmd(0x65u + l * 3u))});
        };
        const auto lu = direction(ls & 3u), lv = direction((ls >> 8u) & 3u);
        const float shade[8]{lu[0], lu[1], lu[2], 0.0f, lv[0], lv[1], lv[2], 0.0f};
        uniform4v(g_program.shade, shade, 2);
    }
    static bool lighting_has_color = false;
    if (lit && (g_lighting_dirty || lighting_has_color != (col >= 4u))) {
        g_lighting_dirty = false;
        lighting_has_color = col >= 4u;
        const auto lighting = lighting_uniforms(lighting_has_color);
        uniform4v(g_program.lighting, lighting.data(), 30);
    }
}

// Records the batch as one Draw: vertex source, attribute layout, indices.
void bind_vertices() {
    const std::uint32_t vtype = g_batch_vtype;
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    std::array<AttribSetup, 4> attribs{};
    attribs[0] = {3, component_type(pos, true), 0u, g_layout.pos, {}};
    if (tc != 0u) attribs[1] = {2, component_type(tc, false), 0u, g_layout.tc, {}};
    else attribs[1] = {0, 0u, 0u, 0u, {0.0f, 0.0f, 0.0f, 1.0f}};
    if (col == 7u) {
        attribs[2] = {4, GL_UNSIGNED_BYTE, 1u, g_layout.col, {}};
    } else if (col >= 4u) {
        attribs[2] = {1, GL_UNSIGNED_SHORT, 0u, g_layout.col, {}};
    } else {
        // No vertex colour: the material ambient colour.
        const auto ambient = rgb_command(0x55u, static_cast<float>(cmd(0x58u) & 0xFFu) / 255.0f);
        attribs[2] = {0, 0u, 0u, 0u, {ambient[0], ambient[1], ambient[2], ambient[3]}};
    }
    if (nrm != 0u) attribs[3] = {3, component_type(nrm, true), 0u, g_layout.nrm, {}};
    else attribs[3] = {0, 0u, 0u, 0u, {0.0f, 0.0f, 1.0f, 1.0f}};

    // In-place vertices stay where they are (the frame keeps their snapshot;
    // static ones are guest memory); a copied batch goes into the frame.
    const std::size_t bytes = static_cast<std::size_t>(g_batch_count) * g_layout.stride;
    std::uint32_t in_frame = 1u;
    std::uint32_t source = 0u;
    if (g_batch_in_place != nullptr) {
        in_frame = 0u;
        const auto pointer = reinterpret_cast<std::uintptr_t>(g_batch_in_place);
        source = record_data(&pointer, sizeof(pointer));
    } else {
        source = record_data(g_batch.data(), bytes);
    }
    record(Op::Draw, {in_frame, source, static_cast<std::uint32_t>(bytes), g_layout.stride,
                      record_data(attribs.data(), sizeof(attribs)),
                      record_data(g_batch_indices.data(), g_batch_indices.size() * sizeof(std::uint16_t)),
                      static_cast<std::uint32_t>(g_batch_indices.size())});
}

// Snapshot record: header, then the index bytes (a LOAD_CLUT record keeps the
// palette there); the vertex bytes are in Snapshot::vertices. UINT32_MAX marks
// a part the emulation thread could not read.
struct SnapshotHeader {
    std::uint32_t index_bytes{UINT32_MAX}, vertex_bytes{UINT32_MAX}, vertex_offset{};
    std::uint32_t index_in_guest{};  // the index part (palette) is read from guest memory
};

const std::uint8_t *g_snapshot{};
const std::uint8_t *g_snapshot_vertices{};

// Burnout keeps its track and car geometry where it was loaded; only the
// display-list buffers and a small heap (0x09C00000-0x09DFFFFF) hold vertices
// rebuilt every frame (measured: about 3% of the bytes drawn change between
// frames). Static vertices are not copied: the worker reads guest memory.
constexpr std::uint32_t kInGuestMemory = UINT32_MAX;

bool static_vertices(std::uint32_t address) noexcept { return address >= 0x08800000u && address < 0x09C00000u; }

// `bytes` of guest memory at `address`, from the current snapshot record when
// there is one (`vertices` picks its part).
const std::uint8_t *guest_bytes(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t bytes, bool vertices) {
    if (g_snapshot == nullptr) return memory.raw_pointer(address, bytes);
    SnapshotHeader header;
    std::memcpy(&header, g_snapshot, sizeof(header));
    const std::uint32_t have = vertices ? header.vertex_bytes : header.index_bytes;
    if (have == UINT32_MAX || have < bytes) return nullptr;
    if (!vertices) return header.index_in_guest != 0u ? memory.raw_pointer(address, bytes) : g_snapshot + sizeof(header);
    if (header.vertex_offset == kInGuestMemory) return memory.raw_pointer(address, bytes);
    return g_snapshot_vertices + header.vertex_offset;
}

bool decode_prim(psprecomp::GuestMemory &memory, std::uint32_t data) {
    phase = 1u;
    const std::uint32_t primitive = (data >> 16u) & 7u;
    const std::uint32_t count = data & 0xFFFFu;
    const std::uint32_t vtype = cmd(0x12u);
    const std::uint32_t weight = (vtype >> 9u) & 3u, index = (vtype >> 11u) & 3u;
    const std::uint32_t weights = weight != 0u ? ((vtype >> 14u) & 7u) + 1u : 0u;
    const std::uint32_t morphs = ((vtype >> 18u) & 7u) + 1u;
    const bool through = (vtype & 0x800000u) != 0u;

    // The vertex type rarely changes between PRIMs.
    static std::uint32_t layout_vtype = UINT32_MAX;
    static VertexLayout layout;
    if (vtype != layout_vtype) {
        layout_vtype = vtype;
        layout = vertex_layout(vtype);
    }
    const std::uint32_t stride = layout.stride;

    if (weights != 0u || morphs > 1u || primitive < 3u) {  // skinning/morphing/points/lines: not yet
        ++g_stats.skipped_primitives;
        if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
        else g_vertex_address += count * stride;
        return false;
    }

    const bool lit = !through && (cmd(0x17u) & 1u) != 0u;

    phase = 2u;
    std::uint32_t max_vertex = 0u;
    static std::vector<std::uint32_t> indices;
    indices.resize(count);
    if (index == 0u) {
        for (std::uint32_t i = 0; i < count; ++i) indices[i] = i;
        max_vertex = count == 0u ? 0u : count - 1u;
    } else {
        const std::uint8_t *index_data = guest_bytes(memory, g_index_address, count * (index == 1u ? 1u : 2u), false);
        if (index_data == nullptr) { ++g_stats.skipped_primitives; return false; }
        for (std::uint32_t i = 0; i < count; ++i) {
            indices[i] = index == 1u ? index_data[i]
                                     : static_cast<std::uint32_t>(index_data[i * 2u] | (index_data[i * 2u + 1u] << 8u));
            max_vertex = std::max(max_vertex, indices[i]);
        }
    }
    phase = 3u;
    const std::uint32_t vertex_count = count == 0u ? 0u : max_vertex + 1u;
    const std::uint8_t *raw = guest_bytes(memory, g_vertex_address, vertex_count * stride, true);
    if (raw == nullptr) { ++g_stats.skipped_primitives; return false; }

    if (!g_batch_indices.empty() && g_batch_vtype != vtype) flush();
    if (g_batch_count == 0u) {
        g_batch_vtype = vtype;
        g_layout = batch_layout(vtype);
    }
    if (lit) g_stats.lit_vertices += vertex_count;
    // Indexed batch: the PRIM's vertices are appended once and triangles index
    // them. Flat shading needs per-triangle colours, so it copies vertices.
    const bool flat = (cmd(0x50u) & 1u) == 0u && (cmd(0xD3u) & 1u) == 0u;
    const std::size_t worst = flat ? static_cast<std::size_t>(count) * 3u
        : primitive == 6u ? static_cast<std::size_t>(count) * 2u : vertex_count;
    if (g_batch_count + worst > 65535u) {
        flush();
        g_batch_vtype = vtype;
        g_layout = batch_layout(vtype);
    }
    const BatchLayout &out = g_layout;
    const auto vertex = [&](std::uint32_t i) { return raw + static_cast<std::size_t>(i) * stride; };
    // Snapshot vertices already in the batch layout are drawn where they are,
    // as long as they follow the batch's previous ones.
    const bool in_place = g_snapshot != nullptr && out.same_as_psp && !flat && primitive != 6u;
    const bool follows = g_batch_in_place != nullptr && raw == g_batch_in_place + static_cast<std::size_t>(g_batch_count) * stride;
    if (g_batch_count != 0u && (in_place ? !follows : g_batch_in_place != nullptr)) {
        flush();
        g_batch_vtype = vtype;
        g_layout = batch_layout(vtype);
    }
    if (in_place && g_batch_count == 0u) g_batch_in_place = raw;
    const auto base = g_batch_count;
    // At most three indices per PRIM vertex (strips, fans, sprites).
    const std::size_t index_start = g_batch_indices.size();
    g_batch_indices.resize(index_start + static_cast<std::size_t>(count) * 3u);
    std::uint16_t *next = g_batch_indices.data() + index_start;
    const auto put = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        next[0] = static_cast<std::uint16_t>(a);
        next[1] = static_cast<std::uint16_t>(b);
        next[2] = static_cast<std::uint16_t>(c);
        next += 3;
    };
    phase = 4u;
    if (in_place) {
        g_batch_count += vertex_count;
    } else if (!flat && primitive != 6u) {
        std::uint8_t *dst = append_vertices(vertex_count);
        if (out.same_as_psp) std::memcpy(dst, raw, static_cast<std::size_t>(vertex_count) * stride);
        else
            for (std::uint32_t i = 0; i < vertex_count; ++i) copy_vertex(dst + i * out.stride, vertex(i), out, layout);
    }
    const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (flat) {
            // The last vertex gives the colour (and the normal it is lit with).
            const auto first = static_cast<std::uint16_t>(g_batch_count);
            std::uint8_t *dst = append_vertices(3u);
            copy_vertex(dst, vertex(indices[a]), out, layout);
            copy_vertex(dst + out.stride, vertex(indices[b]), out, layout);
            copy_vertex(dst + out.stride * 2u, vertex(indices[c]), out, layout);
            for (int k = 0; k < 2; ++k) {
                std::memcpy(dst + out.stride * k + out.col, dst + out.stride * 2u + out.col, out.col_size);
                std::memcpy(dst + out.stride * k + out.nrm, dst + out.stride * 2u + out.nrm, out.nrm_size * 3u);
            }
            put(first, first + 1u, first + 2u);
            return;
        }
        put(base + indices[a], base + indices[b], base + indices[c]);
    };
    phase = 5u;
    switch (primitive) {
    case 3u:
        for (std::uint32_t i = 0; i + 2u < count; i += 3u) emit(i, i + 1u, i + 2u);
        break;
    case 4u:
        for (std::uint32_t i = 0; i + 2u < count; ++i) {
            if (i & 1u) emit(i + 1u, i, i + 2u);
            else emit(i, i + 1u, i + 2u);
        }
        break;
    case 5u:
        for (std::uint32_t i = 1; i + 1u < count; ++i) emit(0u, i, i + 1u);
        break;
    case 6u:
        for (std::uint32_t i = 0; i + 1u < count; i += 2u) {
            // Sprite: corners from the two vertices, colour and depth of the
            // second. Corners in order: (a.x, a.y), (b.x, a.y), b, (a.x, b.y).
            const std::uint8_t *a = vertex(indices[i]);
            const auto first = static_cast<std::uint16_t>(g_batch_count);
            std::uint8_t *dst = append_vertices(4u);
            for (std::uint32_t k = 0; k < 4u; ++k) copy_vertex(dst + k * out.stride, vertex(indices[i + 1u]), out, layout);
            const auto take = [&](std::uint32_t corner, bool x, bool y) {
                std::uint8_t *c = dst + corner * out.stride;
                if (x) std::memcpy(c + out.pos, a + layout.pos, out.pos_size);
                if (y) std::memcpy(c + out.pos + out.pos_size, a + layout.pos + out.pos_size, out.pos_size);
                if (x) std::memcpy(c + out.tc, a + layout.tc, out.tc_size);
                if (y) std::memcpy(c + out.tc + out.tc_size, a + layout.tc + out.tc_size, out.tc_size);
            };
            take(0u, true, true);
            take(1u, false, true);
            take(3u, true, false);
            put(first, first + 1u, first + 2u);
            put(first, first + 2u, first + 3u);
        }
        break;
    default: break;
    }
    g_batch_indices.resize(static_cast<std::size_t>(next - g_batch_indices.data()));
    if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
    else g_vertex_address += count * stride;
    phase = 0u;
    return true;
}

// Commands that do not change how the pending batch is drawn.
bool batch_neutral(std::uint32_t op) noexcept {
    switch (op) {
    case 0x00u: case 0x01u: case 0x02u: case 0x04u: case 0x08u: case 0x0Au: case 0x0Bu: case 0x0Cu:
    case 0x0Eu: case 0x0Fu: case 0x10u: case 0x13u: case 0x14u:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- GL thread side

struct GlThread {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Frame> frames;
    bool busy{}, ready{}, ok{};
    std::string error;
} g_gl;

std::vector<GLuint> g_gl_textures, g_gl_framebuffers;  // GL names by id (GL thread)
std::atomic<double> g_gl_busy_ms{0.0}, g_gl_present_ms{0.0};

GLuint gl_texture(GLuint id) {
    if (id >= g_gl_textures.size()) g_gl_textures.resize(id + 1u, 0u);
    if (g_gl_textures[id] == 0u) glGenTextures(1, &g_gl_textures[id]);
    return g_gl_textures[id];
}

void create_target(GLuint framebuffer_id, GLuint texture_id, int width, int height) {
    const GLuint texture = gl_texture(texture_id);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLuint depth = 0u, framebuffer = 0u;
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
#if defined(__vita__)
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
#else
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
#endif
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(0.0);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (framebuffer_id >= g_gl_framebuffers.size()) g_gl_framebuffers.resize(framebuffer_id + 1u, 0u);
    g_gl_framebuffers[framebuffer_id] = framebuffer;
}

void draw(const Frame &frame, const std::uint32_t *a) {
    const std::uint32_t in_frame = a[0], source = a[1], bytes = a[2], stride = a[3], attribs_at = a[4];
    const std::uint32_t indices_at = a[5], index_count = a[6];
    const std::uint8_t *vertices = frame.data.data() + source;
    if (in_frame == 0u) {
        std::uintptr_t pointer;
        std::memcpy(&pointer, vertices, sizeof(pointer));
        vertices = reinterpret_cast<const std::uint8_t *>(pointer);
    }
    std::array<AttribSetup, 4> attribs;
    std::memcpy(attribs.data(), frame.data.data() + attribs_at, sizeof(attribs));
    const void *indices = frame.data.data() + indices_at;
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(vertices);
#if defined(__vita__)
    // One copy into GPU-visible scratch memory, wrapped as a vertex buffer:
    // vitaGL then draws from it instead of copying every attribute stream.
    if (void *gpu = vglAllocFromScratch(bytes)) {
        std::memcpy(gpu, vertices, bytes);
        glBindBuffer(GL_ARRAY_BUFFER, g_vertex_buffer);
        vglBufferData(GL_ARRAY_BUFFER, gpu);
        base = 0u;
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    const std::size_t index_bytes = index_count * sizeof(std::uint16_t);
    if (void *gpu = vglAllocFromScratch(index_bytes)) {
        std::memcpy(gpu, indices, index_bytes);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_index_buffer);
        vglBufferData(GL_ELEMENT_ARRAY_BUFFER, gpu);
        indices = nullptr;
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
#else
    (void)bytes;
#endif
    for (GLuint i = 0; i < 4u; ++i) {
        const AttribSetup &setup = attribs[i];
        if (setup.size == 0) {
            glDisableVertexAttribArray(i);
            glVertexAttrib4f(i, setup.constant[0], setup.constant[1], setup.constant[2], setup.constant[3]);
        } else {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, setup.size, setup.type, setup.normalized != 0u ? GL_TRUE : GL_FALSE,
                                  static_cast<GLsizei>(stride), reinterpret_cast<const void *>(base + setup.offset));
        }
    }
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
}

void present_target(GLuint texture_id, int width) {
#if defined(__vita__)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (texture_id != 0u) {
        const float u = 480.0f / static_cast<float>(width);
        const float quad[16]{-1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, u, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, u, 0.0f};
        glUseProgram(g_present.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_texture(texture_id));
        glUniform1i(g_present.texture, 0);
        glDisableVertexAttribArray(2);
        glDisableVertexAttribArray(3);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    swap_buffers();
#else
    (void)texture_id;
    (void)width;
#endif
}

void capture_target(GLuint framebuffer_id, int height, const std::function<void(std::vector<std::uint8_t> &)> &done) {
    std::vector<std::uint8_t> rgb;
    if (framebuffer_id < g_gl_framebuffers.size() && g_gl_framebuffers[framebuffer_id] != 0u) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_gl_framebuffers[framebuffer_id]);
        std::vector<std::uint8_t> rgba(480u * 272u * 4u);
        glReadPixels(0, height - 272, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        rgb.resize(480u * 272u * 3u);
        for (std::uint32_t y = 0; y < 272u; ++y)
            for (std::uint32_t x = 0; x < 480u; ++x)
                for (int c = 0; c < 3; ++c) rgb[(y * 480u + x) * 3u + c] = rgba[((271u - y) * 480u + x) * 4u + c];
    }
    done(rgb);
}

void replay(const Frame &frame) {
    const std::uint32_t *op = frame.ops.data();
    const std::uint32_t *end = op + frame.ops.size();
    const auto floats = [&](std::uint32_t at) { return reinterpret_cast<const float *>(frame.data.data() + at); };
    while (op < end) {
        switch (static_cast<Op>(*op++)) {
        case Op::Enable: glEnable(op[0]); op += 1; break;
        case Op::Disable: glDisable(op[0]); op += 1; break;
        case Op::BlendFunc: glBlendFunc(op[0], op[1]); op += 2; break;
        case Op::BlendEquation: glBlendEquation(op[0]); op += 1; break;
        case Op::DepthFunc: glDepthFunc(op[0]); op += 1; break;
        case Op::DepthMask: glDepthMask(op[0] != 0u ? GL_TRUE : GL_FALSE); op += 1; break;
        case Op::ColorMask: {
            const GLboolean rgb = op[0] != 0u ? GL_TRUE : GL_FALSE;
            glColorMask(rgb, rgb, rgb, op[1] != 0u ? GL_TRUE : GL_FALSE);
            op += 2;
            break;
        }
        case Op::FrontFace: glFrontFace(op[0]); op += 1; break;
        case Op::Scissor:
            glScissor(static_cast<GLint>(op[0]), static_cast<GLint>(op[1]), static_cast<GLsizei>(op[2]), static_cast<GLsizei>(op[3]));
            op += 4;
            break;
        case Op::Viewport: glViewport(0, 0, static_cast<GLsizei>(op[0]), static_cast<GLsizei>(op[1])); op += 2; break;
        case Op::UseProgram: glUseProgram(op[0]); op += 1; break;
        case Op::BindFramebuffer:
            glBindFramebuffer(GL_FRAMEBUFFER, op[0] < g_gl_framebuffers.size() ? g_gl_framebuffers[op[0]] : 0u);
            op += 1;
            break;
        case Op::BindTexture: glBindTexture(GL_TEXTURE_2D, gl_texture(op[0])); op += 1; break;
        case Op::TexParameter: glTexParameteri(GL_TEXTURE_2D, op[0], static_cast<GLint>(op[1])); op += 2; break;
        case Op::TexImage:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(op[0]), static_cast<GLsizei>(op[1]), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, frame.blobs[op[2]].data());
            op += 3;
            break;
        case Op::Uniform4: glUniform4fv(static_cast<GLint>(op[0]), static_cast<GLsizei>(op[1]), floats(op[2])); op += 3; break;
        case Op::UniformMatrix: glUniformMatrix4fv(static_cast<GLint>(op[0]), 1, GL_FALSE, floats(op[1])); op += 2; break;
        case Op::Draw: draw(frame, op); op += 7; break;
        case Op::CreateTarget:
            create_target(op[0], op[1], static_cast<int>(op[2]), static_cast<int>(op[3]));
            op += 4;
            break;
        case Op::Present: {
            const double started = host_now_ms();
            present_target(op[0], static_cast<int>(op[1]));
            g_gl_present_ms.store(g_gl_present_ms.load() + host_now_ms() - started);
            op += 2;
            break;
        }
        case Op::Capture: capture_target(op[0], static_cast<int>(op[1]), frame.captures[op[2]]); op += 3; break;
        }
    }
}

void gl_thread_main() {
    {
        std::string error;
        bool ok = create_context(error);
        if (ok) trace("compiling the draw shaders");
        ok = ok && link(g_program, kVertexShader, kFragmentShader, false, error);
        if (ok) trace("compiling the present shaders");
        ok = ok && link(g_present, kPresentVertexShader, kPresentFragmentShader, true, error);
#if defined(__vita__)
        if (ok) {
            glGenBuffers(1, &g_vertex_buffer);
            glGenBuffers(1, &g_index_buffer);
        }
#endif
        std::lock_guard lock(g_gl.mutex);
        g_gl.ready = true;
        g_gl.ok = ok;
        g_gl.error = error;
        g_gl.cv.notify_all();
        if (!ok) return;
    }
    for (;;) {
        Frame frame;
        {
            std::unique_lock lock(g_gl.mutex);
            g_gl.cv.wait(lock, [] { return !g_gl.frames.empty(); });
            frame = std::move(g_gl.frames.front());
            g_gl.frames.pop_front();
            g_gl.busy = true;
        }
        const double started = host_now_ms();
        replay(frame);
        // Test knob: a slow GPU side (PC runs of the Vita pacing paths).
        static const int delay_ms = [] {
            const char *value = std::getenv("PSPRECOMP_BURNOUT_GL_DELAY_MS");
            return value != nullptr ? std::atoi(value) : 0;
        }();
        if (delay_ms > 0) host_sleep_ms(static_cast<double>(delay_ms));
        g_gl_busy_ms.store(g_gl_busy_ms.load() + host_now_ms() - started);
        frame = Frame{};  // release snapshots and payloads before reporting idle
        std::lock_guard lock(g_gl.mutex);
        g_gl.busy = false;
        g_gl.cv.notify_all();
    }
}

#if defined(__vita__)
void *gl_thread_entry(void *) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
    trace("GL thread started (core 2)");
    gl_thread_main();
    return nullptr;
}
#endif

// Hands the recorded frame to the GL thread (at most two wait there).
void submit_frame() {
    std::unique_lock lock(g_gl.mutex);
    g_gl.cv.wait(lock, [] { return g_gl.frames.size() < 2u; });
    g_gl.frames.push_back(std::move(g_frame));
    g_frame = Frame{};
    g_gl.cv.notify_all();
}

} // namespace

void trace(const char *message) {
    std::fprintf(stderr, "[gl] %s\n", message);
    std::fflush(stderr);
}

bool initialize(std::string &error) {
    trace("creating the context");
#if defined(__vita__)
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024);  // the shader compiler wants a large stack
    pthread_t thread;
    const int result = pthread_create(&thread, &attributes, gl_thread_entry, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        error = "pthread_create failed for the GL thread";
        return false;
    }
    pthread_detach(thread);
#else
    std::thread(gl_thread_main).detach();
#endif
    std::unique_lock lock(g_gl.mutex);
    g_gl.cv.wait(lock, [] { return g_gl.ready; });
    if (!g_gl.ok) {
        error = g_gl.error;
        return false;
    }
    trace("ready");
    g_commands.fill(0u);
    return true;
}

void finish() {
    if (!g_frame.ops.empty()) submit_frame();
    std::unique_lock lock(g_gl.mutex);
    g_gl.cv.wait(lock, [] { return g_gl.frames.empty() && !g_gl.busy; });
}

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset) {
    g_memory = &memory;
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    // Matrix uploads only end the batch when they change an element: games
    // often reload the matrix that is already there.
    const auto matrix_changes = [&]() {
        const float value = float24(data);
        switch (op) {
        case 0x3Bu: return g_world_cursor < 12u && g_world[g_world_cursor] != value;
        case 0x3Du: return g_view_cursor < 12u && g_view[g_view_cursor] != value;
        case 0x3Fu: return g_projection_cursor < 16u && g_projection[g_projection_cursor] != value;
        case 0x41u: return g_texgen_cursor < 12u && g_texgen[g_texgen_cursor] != value;
        case 0x2Bu: return true;
        default: return false;
        }
    };
    if (!batch_neutral(op) && (matrix_changes() || g_commands[op] != word || op == 0xC4u || op == 0xEAu)) flush();
    if (g_commands[op] != word && ((op >= 0x18u && op <= 0x1Bu) || (op >= 0x53u && op <= 0x92u))) g_lighting_dirty = true;
    g_commands[op] = word;
    const auto address = [&]() { return ((base | data) + offset) & 0x0FFFFFFFu; };
    switch (op) {
    case 0x01u: g_vertex_address = address(); break;
    case 0x02u: g_index_address = address(); break;
    case 0x04u:
        ++g_stats.primitives;
        decode_prim(memory, data);
        break;
    case 0x3Au: g_world_cursor = data & 0xFu; break;
    case 0x3Bu: if (g_world_cursor < 12u) g_world[g_world_cursor++] = float24(data); break;
    case 0x3Cu: g_view_cursor = data & 0xFu; break;
    case 0x3Du: if (g_view_cursor < 12u) g_view[g_view_cursor++] = float24(data); break;
    case 0x3Eu: g_projection_cursor = data & 0xFu; break;
    case 0x3Fu: if (g_projection_cursor < 16u) g_projection[g_projection_cursor++] = float24(data); break;
    case 0x40u: g_texgen_cursor = data & 0xFu; break;
    case 0x41u: if (g_texgen_cursor < 12u) g_texgen[g_texgen_cursor++] = float24(data); break;
    case 0xC4u: {  // LOAD_CLUT: the GE copies the palette on chip
        const std::uint32_t clut = (cmd(0xB0u) & 0xFFFFF0u) | ((cmd(0xB1u) << 8u) & 0x0F000000u);
        const std::uint32_t bytes = std::min<std::uint32_t>((data & 0x3Fu) * 32u, 1024u);
        if (const std::uint8_t *p = guest_bytes(memory, clut, bytes, false)) std::memcpy(g_clut.data(), p, bytes);
        g_clut_hash = fnv(g_clut.data(), g_clut.size());
        break;
    }
    default: break;
    }
}

namespace {

// Pointer state of the emulation thread, kept apart from the worker's.
struct SnapshotState {
    std::uint32_t vtype{}, clut_low{}, clut_high{}, vertex_address{}, index_address{};
} g_scan;

void append(std::vector<std::uint8_t> &arena, const std::uint8_t *bytes, std::uint32_t size) {
    arena.insert(arena.end(), bytes, bytes + size);
}

} // namespace

std::uint32_t snapshot_command(const psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base,
                               std::uint32_t offset, Snapshot &snapshot) {
    std::vector<std::uint8_t> &arena = snapshot.records;
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    switch (op) {
    case 0x01u: g_scan.vertex_address = ((base | data) + offset) & 0x0FFFFFFFu; return UINT32_MAX;
    case 0x02u: g_scan.index_address = ((base | data) + offset) & 0x0FFFFFFFu; return UINT32_MAX;
    case 0x12u: g_scan.vtype = data; return UINT32_MAX;
    case 0xB0u: g_scan.clut_low = data; return UINT32_MAX;
    case 0xB1u: g_scan.clut_high = data; return UINT32_MAX;
    case 0x04u: case 0xC4u: break;
    default: return UINT32_MAX;
    }
    const auto record = static_cast<std::uint32_t>(arena.size());
    SnapshotHeader header;
    arena.resize(arena.size() + sizeof(header));
    if (op == 0xC4u) {
        emulation_phase = 5u;
        const std::uint32_t clut = (g_scan.clut_low & 0xFFFFF0u) | ((g_scan.clut_high << 8u) & 0x0F000000u);
        const std::uint32_t bytes = std::min<std::uint32_t>((data & 0x3Fu) * 32u, 1024u);
        if (const std::uint8_t *p = memory.raw_pointer(clut, bytes)) {
            header.index_bytes = bytes;
            // Palettes loaded with the track and cars do not change either.
            if (static_vertices(clut)) header.index_in_guest = 1u;
            else append(arena, p, bytes);
        }
    } else {
        // Same pointer arithmetic as decode_prim.
        const std::uint32_t count = data & 0xFFFFu;
        const std::uint32_t index = (g_scan.vtype >> 11u) & 3u;
        const std::uint32_t stride = vertex_layout(g_scan.vtype).stride;
        std::uint32_t vertex_count = count;
        bool readable = true;
        emulation_phase = 3u;
        if (index != 0u) {
            const std::uint32_t bytes = count * (index == 1u ? 1u : 2u);
            const std::uint8_t *p = memory.raw_pointer(g_scan.index_address, bytes);
            g_scan.index_address += bytes;
            readable = p != nullptr;
            if (readable) {
                header.index_bytes = bytes;
                append(arena, p, bytes);
                std::uint32_t max_vertex = 0u;
                for (std::uint32_t i = 0; i < count; ++i)
                    max_vertex = std::max<std::uint32_t>(max_vertex, index == 1u ? p[i] : (p[i * 2u] | (p[i * 2u + 1u] << 8u)));
                vertex_count = count == 0u ? 0u : max_vertex + 1u;
            }
        }
        emulation_phase = 4u;
        if (readable) {
            const std::uint32_t bytes = vertex_count * stride;
            if (const std::uint8_t *p = memory.raw_pointer(g_scan.vertex_address, bytes)) {
                if (header.index_bytes == UINT32_MAX) header.index_bytes = 0u;
                header.vertex_bytes = bytes;
                if (static_vertices(g_scan.vertex_address)) {
                    header.vertex_offset = kInGuestMemory;
                } else {
                    header.vertex_offset = static_cast<std::uint32_t>(snapshot.vertices.size());
                    append(snapshot.vertices, p, bytes);
                }
            }
        }
        if (index == 0u) g_scan.vertex_address += count * stride;
    }
    std::memcpy(&arena[record], &header, sizeof(header));
    emulation_phase = 2u;
    return record;
}

void use_snapshot(const Snapshot *snapshot, std::uint32_t record) {
    g_snapshot = snapshot != nullptr ? snapshot->records.data() + record : nullptr;
    g_snapshot_vertices = snapshot != nullptr ? snapshot->vertices.data() : nullptr;
}

void end_list(Snapshot &&snapshot) {
    if (g_batch_in_place != nullptr) flush();
    // Recorded draws may point into it until the GL thread has replayed them.
    if (!snapshot.vertices.empty()) g_frame.snapshots.push_back(std::move(snapshot));
}

void set_drawing(bool drawing) {
    if (!drawing) flush();
    g_drawing = drawing;
}

void invalidate_textures(std::uint32_t address, std::uint32_t size) {
    for (auto &[key, texture] : g_textures)
        if (texture.address < address + size && address < texture.address + texture.bytes) texture.stale = true;
}

void present(std::uint32_t address) {
    flush();
    phase = 11u;
    ++g_stats.frames;
    if (g_stats.frames <= 3u) {
        char line[64];
        std::snprintf(line, sizeof(line), "frame %llu", static_cast<unsigned long long>(g_stats.frames));
        trace(line);
    }
    if (g_stats.frames % 300u == 0u) {
        // Per-frame averages since the previous line (worker thread time).
        static Stats last;
        static double last_wall = host_now_ms();
        static double last_busy = 0.0, last_present = 0.0;
        const double wall = host_now_ms();
        const double n = static_cast<double>(g_stats.frames - last.frames);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "frame %llu: %.1f ms/frame wall, decode %.1f, record %.1f, present %.1f, idle %.1f; %.0f PRIM, %.0f batches, %.0f vertices (%.0f lit), %llu texture uploads",
                      static_cast<unsigned long long>(g_stats.frames), (wall - last_wall) / n,
                      (g_stats.list_ms - last.list_ms - (g_stats.draw_ms - last.draw_ms)) / n, (g_stats.draw_ms - last.draw_ms) / n,
                      (g_gl_present_ms.load() - last_present) / n, (g_stats.idle_ms - last.idle_ms) / n,
                      static_cast<double>(g_stats.primitives - last.primitives) / n,
                      static_cast<double>(g_stats.batches - last.batches) / n, static_cast<double>(g_stats.vertices - last.vertices) / n,
                      static_cast<double>(g_stats.lit_vertices - last.lit_vertices) / n,
                      static_cast<unsigned long long>(g_stats.texture_uploads - last.texture_uploads));
        trace(line);
        std::snprintf(line, sizeof(line), "  GL thread busy %.1f ms/frame", (g_gl_busy_ms.load() - last_busy) / n);
        trace(line);
        last = g_stats;
        last_wall = wall;
        last_busy = g_gl_busy_ms.load();
        last_present = g_gl_present_ms.load();
    }
    const auto it = g_targets.find(target_key(address));
    if (!g_drawing) {
        phase = 0u;
        return;  // skipped frame: the screen keeps the previous one
    }
    record(Op::Present, {it != g_targets.end() ? it->second.texture : 0u,
                         it != g_targets.end() ? static_cast<std::uint32_t>(it->second.width) : 0u});
    forget_state();
    submit_frame();
    phase = 0u;
}

void capture(std::uint32_t address, std::function<void(std::vector<std::uint8_t> &rgb)> done) {
    flush();
    const auto it = g_targets.find(target_key(address));
    record(Op::Capture, {it != g_targets.end() ? it->second.framebuffer : 0u,
                         it != g_targets.end() ? static_cast<std::uint32_t>(it->second.height) : 272u,
                         static_cast<std::uint32_t>(g_frame.captures.size())});
    g_frame.captures.push_back(std::move(done));
    forget_state();
}

const Stats &stats() noexcept { return g_stats; }

void account(double list_ms, double idle_ms) noexcept {
    g_stats.list_ms += list_ms;
    g_stats.idle_ms += idle_ms;
}

} // namespace burnout::gl
