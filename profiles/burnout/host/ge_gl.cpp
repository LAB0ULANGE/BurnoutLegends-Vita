#include "ge_gl.hpp"

#include "gl_api.hpp"

#include "burnout_bench.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace burnout::gl {

namespace {

// ---------------------------------------------------------------- shaders

const char *kVertexShader = R"(
attribute vec3 a_position;
attribute vec3 a_texcoord;
attribute vec4 a_color;
uniform mat4 u_world_view;
uniform mat4 u_projection;
uniform vec4 u_viewport;   // scale x, scale y, centre x - offset x, centre y - offset y
uniform vec4 u_depth;      // scale z, centre z, through (1/0), fog enabled (1/0)
uniform vec4 u_target;     // render target width, height
uniform vec2 u_fog;        // PSP fog end, fog scale
varying vec3 v_texcoord;
varying vec4 v_color;
varying float v_fog;
void main() {
    vec3 screen;
    float w = 1.0;
    v_fog = 1.0;
    if (u_depth.z > 0.5) {
        screen = a_position;
    } else {
        vec4 view = u_world_view * vec4(a_position, 1.0);
        vec4 clip = u_projection * view;
        w = clip.w;
        vec3 ndc = clip.xyz / clip.w;
        screen = vec3(ndc.xy * u_viewport.xy + u_viewport.zw, ndc.z * u_depth.x + u_depth.y);
        if (u_depth.w > 0.5) v_fog = clamp((view.z + u_fog.x) * u_fog.y, 0.0, 1.0);
    }
    gl_Position = vec4((screen.x / u_target.x * 2.0 - 1.0) * w, (1.0 - screen.y / u_target.y * 2.0) * w,
                       (screen.z / 65535.0 * 2.0 - 1.0) * w, w);
    v_texcoord = a_texcoord;
    v_color = a_color;
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
        bool pass = true;
        if (func < 0.5) pass = false;
        else if (func < 1.5) pass = true;
        else if (func < 2.5) pass = a == r;
        else if (func < 3.5) pass = a != r;
        else if (func < 4.5) pass = a < r;
        else if (func < 5.5) pass = a <= r;
        else if (func < 6.5) pass = a > r;
        else pass = a >= r;
        if (!pass) discard;
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

struct Vertex {
    float position[3];
    float texcoord[3];
    std::array<std::uint8_t, 4> color;
};

struct RenderTarget {
    GLuint framebuffer{}, texture{}, depth{};
    int width{}, height{};
};

struct Texture {
    GLuint name{};
    std::uint64_t hash{};
    std::uint64_t checked_frame{UINT64_MAX};
};

struct Program {
    GLuint id{};
    GLint world_view{}, projection{}, viewport{}, depth{}, target{}, fog{};
    GLint texmap{}, texenv{}, envcolor{}, alpha{}, fogcolor{}, texture{}, blendfix{}, alphaout{};
};

std::array<std::uint32_t, 256> g_commands{};
std::array<float, 12> g_world{}, g_view{}, g_texgen{};
std::array<float, 16> g_projection{};
std::uint32_t g_world_cursor{}, g_view_cursor{}, g_projection_cursor{}, g_texgen_cursor{};
std::uint32_t g_vertex_address{}, g_index_address{};
std::array<std::uint8_t, 1024> g_clut{};   // palette cached by LOAD_CLUT, as on the GE
std::vector<Vertex> g_batch;
std::vector<std::uint16_t> g_batch_indices;
bool g_batch_through{};
std::unordered_map<std::uint32_t, RenderTarget> g_targets;
std::unordered_map<std::uint64_t, Texture> g_textures;
Program g_program, g_present;
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
    if (!present) glBindAttribLocation(program.id, 2, "a_color");
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
    program.world_view = u("u_world_view");
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

// ---------------------------------------------------------------- render targets

std::uint32_t target_key(std::uint32_t address) noexcept { return address & 0x001FFFF0u; }

RenderTarget &render_target(std::uint32_t key, std::uint32_t stride) {
    auto [it, inserted] = g_targets.try_emplace(key);
    RenderTarget &target = it->second;
    if (!inserted) return target;
    target.width = static_cast<int>(std::max<std::uint32_t>(stride, 64u));
    target.height = 272;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target.width, target.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &target.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, target.width, target.height);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
#if defined(__vita__)
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depth);
#else
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth);
#endif
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(0.0);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
            glBindTexture(GL_TEXTURE_2D, it->second.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
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
    if (paletted) key ^= fnv(g_clut.data(), g_clut.size(), cmd(0xC5u)) * 31u;
    Texture &texture = g_textures[key];
    const bool fresh = texture.name == 0u;
    if (fresh) glGenTextures(1, &texture.name);
    glBindTexture(GL_TEXTURE_2D, texture.name);
    if (texture.checked_frame != g_stats.frames && source != nullptr) {
        texture.checked_frame = g_stats.frames;
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
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            ++g_stats.texture_uploads;
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
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
    if ((cmd(0x21u) & 1u) == 0u || (cmd(0xD3u) & 1u) != 0u) { glDisable(GL_BLEND); return shader; }
    glEnable(GL_BLEND);
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
    glBlendFunc(source, destination);
    static constexpr GLenum equations[6]{GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD};
    glBlendEquation(equations[std::min<std::uint32_t>((blend >> 8u) & 7u, 5u)]);
    return shader;
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
    glUniformMatrix4fv(g_program.world_view, 1, GL_FALSE, world_view);
    glUniformMatrix4fv(g_program.projection, 1, GL_FALSE, g_projection.data());
}

void flush() {
    if (g_batch_indices.empty()) { g_batch.clear(); return; }
    const double started = host_now_ms();
    const std::uint32_t address = cmd(0x9Cu);
    const std::uint32_t stride = cmd(0x9Du) & 0x7FCu;
    RenderTarget &target = render_target(target_key(address), stride);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, target.width, target.height);
    glUseProgram(g_program.id);

    const bool clear_mode = (cmd(0xD3u) & 1u) != 0u;
    const bool through = g_batch_through;
    upload_matrices();
    glUniform4f(g_program.viewport, float24(cmd(0x42u)), float24(cmd(0x43u)),
                float24(cmd(0x45u)) - static_cast<float>(cmd(0x4Cu) & 0xFFFFu) / 16.0f,
                float24(cmd(0x46u)) - static_cast<float>(cmd(0x4Du) & 0xFFFFu) / 16.0f);
    const bool fog = !through && !clear_mode && (cmd(0x1Fu) & 1u) != 0u;
    glUniform4f(g_program.depth, float24(cmd(0x44u)), float24(cmd(0x47u)), through ? 1.0f : 0.0f, fog ? 1.0f : 0.0f);
    glUniform4f(g_program.target, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 0.0f);
    glUniform2f(g_program.fog, float24(cmd(0xCDu)), float24(cmd(0xCEu)));
    const std::uint32_t fog_color = cmd(0xCFu);
    glUniform4f(g_program.fogcolor, static_cast<float>(fog_color & 0xFFu) / 255.0f,
                static_cast<float>((fog_color >> 8u) & 0xFFu) / 255.0f, static_cast<float>((fog_color >> 16u) & 0xFFu) / 255.0f, 1.0f);

    const bool textured = !clear_mode && (cmd(0x1Eu) & 1u) != 0u;
    if (textured) {
        glActiveTexture(GL_TEXTURE0);
        const double texture_started = host_now_ms();
        const auto texmap = bind_texture(*g_memory);
        g_stats.texture_ms += host_now_ms() - texture_started;
        glUniform4f(g_program.texmap, texmap[0], texmap[1], texmap[2], texmap[3]);
        glUniform1i(g_program.texture, 0);
    }
    const std::uint32_t texfunc = cmd(0xC9u);
    glUniform4f(g_program.texenv, textured ? 1.0f : 0.0f, static_cast<float>(std::min<std::uint32_t>(texfunc & 7u, 4u)),
                (texfunc & 0x100u) != 0u ? 1.0f : 0.0f, (texfunc & 0x10000u) != 0u ? 1.0f : 0.0f);
    const std::uint32_t env = cmd(0xCAu);
    glUniform4f(g_program.envcolor, static_cast<float>(env & 0xFFu) / 255.0f, static_cast<float>((env >> 8u) & 0xFFu) / 255.0f,
                static_cast<float>((env >> 16u) & 0xFFu) / 255.0f, 1.0f);
    const std::uint32_t alpha_test = cmd(0xDBu);
    const bool alpha_enabled = !clear_mode && (cmd(0x22u) & 1u) != 0u;
    glUniform4f(g_program.alpha, static_cast<float>(alpha_test & 7u), static_cast<float>((alpha_test >> 8u) & 0xFFu),
                alpha_enabled ? 1.0f : 0.0f, 0.0f);

    // Scissor (inclusive PSP rectangle, y down) in GL window coordinates.
    const std::uint32_t s1 = cmd(0xD4u), s2 = cmd(0xD5u);
    const int x1 = static_cast<int>(s1 & 0x3FFu), y1 = static_cast<int>((s1 >> 10u) & 0x3FFu);
    const int x2 = static_cast<int>(s2 & 0x3FFu), y2 = static_cast<int>((s2 >> 10u) & 0x3FFu);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x1, target.height - (y2 + 1), std::max(0, x2 - x1 + 1), std::max(0, y2 - y1 + 1));

    if (clear_mode) {
        const std::uint32_t clear = cmd(0xD3u);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask((clear & 0x400u) != 0u ? GL_TRUE : GL_FALSE);
        const GLboolean color = (clear & 0x100u) != 0u ? GL_TRUE : GL_FALSE;
        glColorMask(color, color, color, (clear & 0x200u) != 0u ? GL_TRUE : GL_FALSE);
        glDisable(GL_CULL_FACE);
        glUniform4f(g_program.blendfix, 1.0f, 1.0f, 1.0f, 0.0f);
        glUniform4f(g_program.alphaout, 0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        const BlendShader blend = set_blend();
        glUniform4f(g_program.blendfix, blend.scale[0], blend.scale[1], blend.scale[2], blend.scaled ? 1.0f : 0.0f);
        glUniform4f(g_program.alphaout, blend.alpha >= 0.0f ? 1.0f : 0.0f, std::max(blend.alpha, 0.0f), 0.0f, 0.0f);
        if ((cmd(0x23u) & 1u) != 0u) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(compare(cmd(0xDEu)));
        } else {
            // Depth writes still happen with the test off.
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
        }
        glDepthMask((cmd(0xE7u) & 1u) == 0u ? GL_TRUE : GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        static const bool no_cull = std::getenv("PSPRECOMP_BURNOUT_NOCULL") != nullptr;
        if (!no_cull && !through && (cmd(0x1Du) & 1u) != 0u) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace((cmd(0x9Bu) & 1u) != 0u ? GL_CW : GL_CCW);  // y is flipped on the way to GL
        } else {
            glDisable(GL_CULL_FACE);
        }
    }

    const auto *base = reinterpret_cast<const std::uint8_t *>(g_batch.data());
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, position));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, texcoord));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), base + offsetof(Vertex, color));
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g_batch_indices.size()), GL_UNSIGNED_SHORT, g_batch_indices.data());
    ++g_stats.batches;
    g_stats.vertices += g_batch.size();
    g_batch.clear();
    g_batch_indices.clear();
    g_stats.draw_ms += host_now_ms() - started;
}

// ---------------------------------------------------------------- vertices

struct Decoded {
    float position[3]{};
    float normal[3]{0.0f, 0.0f, 1.0f};
    float texcoord[2]{};
    std::array<std::uint8_t, 4> color{255u, 255u, 255u, 255u};
};

std::array<float, 3> world_point(const float *p, bool translate) {
    const float w = translate ? 1.0f : 0.0f;
    return {p[0] * g_world[0] + p[1] * g_world[3] + p[2] * g_world[6] + w * g_world[9],
            p[0] * g_world[1] + p[1] * g_world[4] + p[2] * g_world[7] + w * g_world[10],
            p[0] * g_world[2] + p[1] * g_world[5] + p[2] * g_world[8] + w * g_world[11]};
}

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

// PSP vertex lighting (the model the VCS software renderer implements). The
// register state is decoded once per PRIM.
struct LightSetup {
    struct Light {
        bool enabled{};
        std::uint32_t computation{}, type{};
        std::array<float, 3> vector{}, attenuation{}, direction{};
        float cutoff{}, exponent{};
        std::array<float, 4> ambient{}, diffuse{}, specular{};
    };
    std::uint32_t update{};
    std::array<float, 4> ambient{}, diffuse{}, specular{}, global{}, emissive{};
    float exponent{};
    std::array<Light, 4> lights{};
};

LightSetup prepare_lighting(bool has_color) {
    LightSetup setup;
    setup.update = cmd(0x53u) & (has_color ? 7u : 0u);
    const float material_alpha = static_cast<float>(cmd(0x58u) & 0xFFu) / 255.0f;
    setup.ambient = rgb_command(0x55u, material_alpha);
    setup.diffuse = rgb_command(0x56u, material_alpha);
    setup.specular = rgb_command(0x57u, material_alpha);
    setup.global = rgb_command(0x5Cu, static_cast<float>(cmd(0x5Du) & 0xFFu) / 255.0f);
    setup.emissive = rgb_command(0x54u, 0.0f);
    setup.exponent = float24(cmd(0x5Bu));
    if (!std::isfinite(setup.exponent) || setup.exponent < 0.0f) setup.exponent = 0.0f;
    for (std::uint32_t l = 0; l < 4u; ++l) {
        LightSetup::Light &light = setup.lights[l];
        light.enabled = (cmd(0x18u + l) & 1u) != 0u;
        if (!light.enabled) continue;
        const std::uint32_t type_data = cmd(0x5Fu + l);
        light.computation = type_data & 3u;
        light.type = (type_data >> 8u) & 3u;
        light.vector = {float24(cmd(0x63u + l * 3u)), float24(cmd(0x64u + l * 3u)), float24(cmd(0x65u + l * 3u))};
        if (light.type == 0u) light.vector = normalized(light.vector);
        light.attenuation = {float24(cmd(0x7Bu + l * 3u)), float24(cmd(0x7Cu + l * 3u)), float24(cmd(0x7Du + l * 3u))};
        light.direction = normalized({float24(cmd(0x6Fu + l * 3u)), float24(cmd(0x70u + l * 3u)), float24(cmd(0x71u + l * 3u))});
        light.cutoff = float24(cmd(0x8Bu + l));
        light.exponent = float24(cmd(0x87u + l));
        light.ambient = rgb_command(0x8Fu + l * 3u, 1.0f);
        light.diffuse = rgb_command(0x90u + l * 3u, 1.0f);
        light.specular = rgb_command(0x91u + l * 3u, 1.0f);
    }
    return setup;
}

std::array<std::uint8_t, 4> light(const Decoded &v, const LightSetup &setup) {
    const std::array<float, 4> vertex{v.color[0] / 255.0f, v.color[1] / 255.0f, v.color[2] / 255.0f, v.color[3] / 255.0f};
    const auto &ambient = (setup.update & 1u) ? vertex : setup.ambient;
    const auto &diffuse = (setup.update & 2u) ? vertex : setup.diffuse;
    const auto &specular = (setup.update & 4u) ? vertex : setup.specular;
    std::array<float, 4> result = setup.emissive;
    for (int i = 0; i < 3; ++i) result[i] += ambient[i] * setup.global[i];
    result[3] = ambient[3] * setup.global[3];
    const auto position = world_point(v.position, true);
    const auto normal = normalized(world_point(v.normal, false));
    for (const LightSetup::Light &l : setup.lights) {
        if (!l.enabled) continue;
        std::array<float, 3> to_light = l.vector;
        float factor = 1.0f;
        if (l.type != 0u) {
            for (int i = 0; i < 3; ++i) to_light[i] -= position[i];
            const float distance = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] + to_light[2] * to_light[2]);
            const float d = l.attenuation[0] + l.attenuation[1] * distance + l.attenuation[2] * distance * distance;
            factor = d > 0.0f ? std::clamp(1.0f / d, 0.0f, 1.0f) : 0.0f;
            to_light = normalized(to_light);
        }
        if (l.type >= 2u) {
            const float spot = l.direction[0] * to_light[0] + l.direction[1] * to_light[1] + l.direction[2] * to_light[2];
            factor *= spot >= l.cutoff ? std::max(0.0f, std::pow(std::max(spot, 0.0f), l.exponent)) : 0.0f;
        }
        for (int i = 0; i < 3; ++i) result[i] += l.ambient[i] * ambient[i] * factor;
        float lambert = to_light[0] * normal[0] + to_light[1] * normal[1] + to_light[2] * normal[2];
        if (l.computation == 2u) lambert = std::pow(std::max(lambert, 0.0f), setup.exponent);
        if (lambert > 0.0f)
            for (int i = 0; i < 3; ++i) result[i] += l.diffuse[i] * diffuse[i] * factor * lambert;
        if (l.computation == 1u && lambert >= 0.0f) {
            const auto half = normalized({to_light[0], to_light[1], to_light[2] + 1.0f});
            const float sp = std::pow(std::max(half[0] * normal[0] + half[1] * normal[1] + half[2] * normal[2], 0.0f), setup.exponent);
            for (int i = 0; i < 3; ++i) result[i] += l.specular[i] * specular[i] * factor * sp;
        }
    }
    std::array<std::uint8_t, 4> out{};
    for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(std::clamp(result[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return out;
}

bool decode_prim(psprecomp::GuestMemory &memory, std::uint32_t data) {
    const std::uint32_t primitive = (data >> 16u) & 7u;
    const std::uint32_t count = data & 0xFFFFu;
    const std::uint32_t vtype = cmd(0x12u);
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const std::uint32_t weight = (vtype >> 9u) & 3u, index = (vtype >> 11u) & 3u;
    const std::uint32_t weights = weight != 0u ? ((vtype >> 14u) & 7u) + 1u : 0u;
    const std::uint32_t morphs = ((vtype >> 18u) & 7u) + 1u;
    const bool through = (vtype & 0x800000u) != 0u;

    // Layout: weights, texcoord, colour, normal, position; each aligned to its size.
    static constexpr std::uint32_t sizes_tc[4]{0, 1, 2, 4}, sizes_col[8]{0, 0, 0, 0, 2, 2, 2, 4};
    static constexpr std::uint32_t sizes_nrm[4]{0, 1, 2, 4}, sizes_pos[4]{0, 1, 2, 4}, sizes_w[4]{0, 1, 2, 4};
    std::uint32_t offset = 0u, alignment = 1u;
    const auto place = [&](std::uint32_t size, std::uint32_t n) {
        offset = (offset + size - 1u) & ~(size - 1u);
        const std::uint32_t at = offset;
        offset += size * n;
        alignment = std::max(alignment, size);
        return at;
    };
    if (weights != 0u) place(sizes_w[weight], weights);
    const std::uint32_t tc_offset = tc ? place(sizes_tc[tc], 2u) : 0u;
    const std::uint32_t col_offset = col >= 4u ? place(sizes_col[col], 1u) : 0u;
    const std::uint32_t nrm_offset = nrm ? place(sizes_nrm[nrm], 3u) : 0u;
    const std::uint32_t pos_offset = place(std::max<std::uint32_t>(sizes_pos[pos], 1u), 3u);
    const std::uint32_t stride = (offset + alignment - 1u) & ~(alignment - 1u);

    if (weights != 0u || morphs > 1u || primitive < 3u) {  // skinning/morphing/points/lines: not yet
        ++g_stats.skipped_primitives;
        std::uint32_t used = count;
        if (index != 0u) {
            std::uint32_t max_index = 0u;
            for (std::uint32_t i = 0; i < count; ++i)
                max_index = std::max<std::uint32_t>(max_index, index == 1u ? memory.load8(g_index_address + i)
                                                                            : memory.load16(g_index_address + i * 2u));
            g_index_address += count * (index == 1u ? 1u : 2u);
            used = 0u;
            (void)max_index;
        }
        g_vertex_address += used * stride;
        return false;
    }

    const float tc_scale = through ? 1.0f : (tc == 1u ? 1.0f / 128.0f : tc == 2u ? 1.0f / 32768.0f : 1.0f);
    const float pos_scale = through ? 1.0f : (pos == 1u ? 1.0f / 128.0f : pos == 2u ? 1.0f / 32768.0f : 1.0f);
    const float nrm_scale = nrm == 1u ? 1.0f / 127.0f : nrm == 2u ? 1.0f / 32767.0f : 1.0f;
    const bool lit = !through && (cmd(0x17u) & 1u) != 0u;
    const std::uint32_t map_mode = cmd(0xC0u) & 3u, map_source = (cmd(0xC0u) >> 8u) & 3u;
    const float su = float24(cmd(0x48u)), sv = float24(cmd(0x49u)), ou = float24(cmd(0x4Au)), ov = float24(cmd(0x4Bu));
    const float texture_width = static_cast<float>(1u << std::min<std::uint32_t>(cmd(0xB8u) & 0xFu, 10u));
    const float texture_height = static_cast<float>(1u << std::min<std::uint32_t>((cmd(0xB8u) >> 8u) & 0xFu, 10u));
    const auto ambient_material = rgb_command(0x55u, static_cast<float>(cmd(0x58u) & 0xFFu) / 255.0f);
    const LightSetup lighting = lit ? prepare_lighting(col >= 4u) : LightSetup{};

    std::uint32_t max_vertex = 0u;
    static std::vector<std::uint32_t> indices;
    indices.resize(count);
    if (index == 0u) {
        for (std::uint32_t i = 0; i < count; ++i) indices[i] = i;
        max_vertex = count == 0u ? 0u : count - 1u;
    } else {
        const std::uint8_t *index_data = memory.raw_pointer(g_index_address, count * (index == 1u ? 1u : 2u));
        if (index_data == nullptr) { ++g_stats.skipped_primitives; return false; }
        for (std::uint32_t i = 0; i < count; ++i) {
            indices[i] = index == 1u ? index_data[i]
                                     : static_cast<std::uint32_t>(index_data[i * 2u] | (index_data[i * 2u + 1u] << 8u));
            max_vertex = std::max(max_vertex, indices[i]);
        }
    }
    const std::uint32_t vertex_count = count == 0u ? 0u : max_vertex + 1u;
    const std::uint8_t *raw = memory.raw_pointer(g_vertex_address, static_cast<std::size_t>(vertex_count) * stride);
    if (raw == nullptr) { ++g_stats.skipped_primitives; return false; }

    static std::vector<Vertex> vertices;
    vertices.resize(vertex_count);
    for (std::uint32_t i = 0; i < vertex_count; ++i) {
        const std::uint8_t *v = raw + static_cast<std::size_t>(i) * stride;
        Decoded d;
        const auto s8 = [](const std::uint8_t *p) { return static_cast<float>(static_cast<std::int8_t>(p[0])); };
        const auto s16 = [](const std::uint8_t *p) { std::int16_t x; std::memcpy(&x, p, 2); return static_cast<float>(x); };
        const auto u16 = [](const std::uint8_t *p) { std::uint16_t x; std::memcpy(&x, p, 2); return static_cast<float>(x); };
        const auto f32 = [](const std::uint8_t *p) { float x; std::memcpy(&x, p, 4); return x; };
        for (int k = 0; k < 3; ++k) {
            const std::uint8_t *p = v + pos_offset + k * sizes_pos[pos];
            d.position[k] = pos == 1u ? s8(p) * pos_scale
                : pos == 2u ? ((through && k == 2) ? u16(p) : s16(p) * pos_scale)
                : f32(p);
        }
        if (nrm)
            for (int k = 0; k < 3; ++k) {
                const std::uint8_t *p = v + nrm_offset + k * sizes_nrm[nrm];
                d.normal[k] = nrm == 1u ? s8(p) * nrm_scale : nrm == 2u ? s16(p) * nrm_scale : f32(p);
            }
        if (tc)
            for (int k = 0; k < 2; ++k) {
                const std::uint8_t *p = v + tc_offset + k * sizes_tc[tc];
                d.texcoord[k] = tc == 1u ? static_cast<float>(p[0]) * tc_scale : tc == 2u ? u16(p) * tc_scale : f32(p);
            }
        if (col >= 4u) {
            const std::uint8_t *p = v + col_offset;
            if (col == 7u) d.color = {p[0], p[1], p[2], p[3]};
            else d.color = color16(static_cast<std::uint16_t>(p[0] | (p[1] << 8u)), col - 4u);
        } else {
            for (int k = 0; k < 4; ++k)
                d.color[k] = static_cast<std::uint8_t>(std::clamp(ambient_material[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        Vertex &out = vertices[i];
        std::memcpy(out.position, d.position, sizeof(out.position));
        out.color = lit ? light(d, lighting) : d.color;
        out.texcoord[2] = 1.0f;
        if (through) {
            out.texcoord[0] = d.texcoord[0] / texture_width;
            out.texcoord[1] = d.texcoord[1] / texture_height;
        } else if (map_mode == 1u) {
            // Texture matrix projection.
            std::array<float, 3> s{d.position[0], d.position[1], d.position[2]};
            if (map_source == 1u) s = {d.texcoord[0], d.texcoord[1], 0.0f};
            else if (map_source == 2u) s = normalized({d.normal[0], d.normal[1], d.normal[2]});
            else if (map_source == 3u) s = {d.normal[0], d.normal[1], d.normal[2]};
            out.texcoord[0] = s[0] * g_texgen[0] + s[1] * g_texgen[3] + s[2] * g_texgen[6] + g_texgen[9];
            out.texcoord[1] = s[0] * g_texgen[1] + s[1] * g_texgen[4] + s[2] * g_texgen[7] + g_texgen[10];
            out.texcoord[2] = s[0] * g_texgen[2] + s[1] * g_texgen[5] + s[2] * g_texgen[8] + g_texgen[11];
        } else if (map_mode == 2u) {
            // Environment (shade) mapping from two light directions.
            const auto n = normalized(world_point(d.normal, false));
            const std::uint32_t ls = cmd(0xC1u);
            const auto light_dir = [&](std::uint32_t l) {
                return normalized({float24(cmd(0x63u + l * 3u)), float24(cmd(0x64u + l * 3u)), float24(cmd(0x65u + l * 3u))});
            };
            const auto lu = light_dir(ls & 3u), lv = light_dir((ls >> 8u) & 3u);
            out.texcoord[0] = (1.0f + n[0] * lu[0] + n[1] * lu[1] + n[2] * lu[2]) * 0.5f;
            out.texcoord[1] = (1.0f + n[0] * lv[0] + n[1] * lv[1] + n[2] * lv[2]) * 0.5f;
        } else {
            out.texcoord[0] = d.texcoord[0] * su + ou;
            out.texcoord[1] = d.texcoord[1] * sv + ov;
        }
    }

    if (!g_batch.empty() && g_batch_through != through) flush();
    g_batch_through = through;
    // Indexed batch: the PRIM's vertices are appended once and triangles index
    // them. Flat shading needs per-triangle colours, so it copies vertices.
    const bool flat = (cmd(0x50u) & 1u) == 0u && (cmd(0xD3u) & 1u) == 0u;
    const std::size_t worst = flat ? static_cast<std::size_t>(count) * 3u
        : primitive == 6u ? static_cast<std::size_t>(count) * 2u : vertex_count;
    if (g_batch.size() + worst > 65535u) flush();
    const auto base = static_cast<std::uint32_t>(g_batch.size());
    if (!flat && primitive != 6u) g_batch.insert(g_batch.end(), vertices.begin(), vertices.end());
    const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (flat) {
            const auto first = static_cast<std::uint16_t>(g_batch.size());
            Vertex va = vertices[indices[a]], vb = vertices[indices[b]], vc = vertices[indices[c]];
            va.color = vb.color = vc.color;
            g_batch.insert(g_batch.end(), {va, vb, vc});
            g_batch_indices.insert(g_batch_indices.end(),
                                   {first, static_cast<std::uint16_t>(first + 1u), static_cast<std::uint16_t>(first + 2u)});
            return;
        }
        g_batch_indices.insert(g_batch_indices.end(), {static_cast<std::uint16_t>(base + indices[a]),
                                                       static_cast<std::uint16_t>(base + indices[b]),
                                                       static_cast<std::uint16_t>(base + indices[c])});
    };
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
            // Sprite: corners from the two vertices, colour and depth of the second.
            const Vertex &a = vertices[indices[i]], &b = vertices[indices[i + 1u]];
            Vertex c00 = b, c10 = b, c01 = b, c11 = b;
            c00.position[0] = a.position[0]; c00.position[1] = a.position[1];
            c00.texcoord[0] = a.texcoord[0]; c00.texcoord[1] = a.texcoord[1];
            c10.position[1] = a.position[1]; c10.texcoord[1] = a.texcoord[1];
            c01.position[0] = a.position[0]; c01.texcoord[0] = a.texcoord[0];
            const auto first = static_cast<std::uint16_t>(g_batch.size());
            g_batch.insert(g_batch.end(), {c00, c10, c11, c01});
            g_batch_indices.insert(g_batch_indices.end(),
                                   {first, static_cast<std::uint16_t>(first + 1u), static_cast<std::uint16_t>(first + 2u), first,
                                    static_cast<std::uint16_t>(first + 2u), static_cast<std::uint16_t>(first + 3u)});
        }
        break;
    default: break;
    }
    if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
    else g_vertex_address += count * stride;
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

} // namespace

bool initialize(std::string &error) {
    if (!create_context(error)) return false;
    if (!link(g_program, kVertexShader, kFragmentShader, false, error)) return false;
    if (!link(g_present, kPresentVertexShader, kPresentFragmentShader, true, error)) return false;
    g_commands.fill(0u);
    return true;
}

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset) {
    g_memory = &memory;
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    const bool matrix_data = op == 0x2Bu || op == 0x3Bu || op == 0x3Du || op == 0x3Fu || op == 0x41u;
    if (!batch_neutral(op) && (matrix_data || g_commands[op] != word || op == 0xC4u || op == 0xEAu)) flush();
    g_commands[op] = word;
    const auto address = [&]() { return ((base | data) + offset) & 0x0FFFFFFFu; };
    switch (op) {
    case 0x01u: g_vertex_address = address(); break;
    case 0x02u: g_index_address = address(); break;
    case 0x04u:
        ++g_stats.primitives;
        {
            const double started = host_now_ms();
            decode_prim(memory, data);
            g_stats.decode_ms += host_now_ms() - started;
        }
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
        if (const std::uint8_t *p = memory.raw_pointer(clut, bytes)) std::memcpy(g_clut.data(), p, bytes);
        break;
    }
    default: break;
    }
}

void invalidate_textures(std::uint32_t address, std::uint32_t size) {
    (void)address;
    (void)size;
    // Texture contents are re-hashed once per frame, so new uploads are seen.
}

void present(std::uint32_t address) {
    flush();
    ++g_stats.frames;
    const double started = host_now_ms();
    const auto it = g_targets.find(target_key(address));
#if defined(__vita__)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (it != g_targets.end()) {
        const RenderTarget &target = it->second;
        const float u = 480.0f / static_cast<float>(target.width);
        const float quad[16]{-1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, u, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, u, 0.0f};
        glUseProgram(g_present.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glUniform1i(g_present.texture, 0);
        glDisableVertexAttribArray(2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    swap_buffers();
#else
    (void)it;
#endif
    g_stats.present_ms += host_now_ms() - started;
}

bool read_frame(std::uint32_t address, std::vector<std::uint8_t> &rgb) {
    flush();
    const auto it = g_targets.find(target_key(address));
    if (it == g_targets.end()) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, it->second.framebuffer);
    std::vector<std::uint8_t> rgba(480u * 272u * 4u);
    glReadPixels(0, it->second.height - 272, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    rgb.resize(480u * 272u * 3u);
    for (std::uint32_t y = 0; y < 272u; ++y)
        for (std::uint32_t x = 0; x < 480u; ++x)
            for (int c = 0; c < 3; ++c) rgb[(y * 480u + x) * 3u + c] = rgba[((271u - y) * 480u + x) * 4u + c];
    return true;
}

const Stats &stats() noexcept { return g_stats; }

} // namespace burnout::gl
