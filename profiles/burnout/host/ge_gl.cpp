#include "ge_gl.hpp"

#include "gl_api.hpp"

#include "burnout_bench.hpp"
#include "thread_sync.hpp"

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
#include <unordered_set>
#if defined(_MSC_VER)
#include <xmmintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#endif

namespace burnout::gl {

volatile std::uint32_t phase{};
volatile std::uint32_t emulation_phase{};
double io_read_ms{}, io_read_max_ms{};

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
uniform vec4 u_colortest;  // colour test reference (0-255), w: function (1 = off)
uniform vec4 u_colormask;  // channels the colour test compares (0 or 1)
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
    if (u_colortest.w < 0.5 || u_colortest.w > 1.5) {
        bool equal_rgb = all(equal(floor(c.rgb * 255.0 + 0.5) * u_colormask.rgb, u_colortest.rgb * u_colormask.rgb));
        if (u_colortest.w < 0.5 || (u_colortest.w < 2.5 ? !equal_rgb : equal_rgb)) discard;
    }
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

// The same pipeline specialised by preprocessor defines (see variant_defines):
// one small program per combination of modes the game uses, with no branches
// on uniforms and, unless the texture matrix projects, the texture
// coordinates finished per vertex so the fragment shader samples them
// directly. The SGX runs these much faster than the shaders above, which
// remain the fallback.
const char *kVariantVertexShader = R"(
attribute vec3 a_position;
attribute vec2 a_texcoord;
attribute vec4 a_color;
attribute vec3 a_normal;
uniform mat4 u_world;
uniform mat4 u_world_view;
uniform mat4 u_projection;
uniform vec4 u_viewport;
uniform vec4 u_depth;
uniform vec4 u_target;
uniform vec4 u_fog;
uniform vec4 u_format;
uniform vec4 u_texgen;
uniform vec4 u_uvscale;
uniform vec4 u_texmap;
uniform mat4 u_texmatrix;
uniform vec4 u_shade[2];
uniform vec4 u_lighting[30];
#if TEXTURED
#if PROJECTIVE
varying vec3 v_texcoord;
#else
varying vec2 v_texcoord;
#endif
#endif
varying vec4 v_color;
#if FOG
varying float v_fog;
#endif

vec3 unit(vec3 v) {
    float l = length(v);
    return l > 0.0 ? v / l : vec3(0.0, 0.0, 1.0);
}

float power(float x, float y) {
    return x > 0.0 ? pow(x, y) : (y == 0.0 ? 1.0 : 0.0);
}

#if LIT
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
#endif

void main() {
    vec3 position = a_position * u_format.x;
    vec3 normal = a_normal * u_format.z;
#if COLFMT == 4
    float v = a_color.x;
    vec4 color = vec4(mod(v, 32.0) / 31.0, mod(floor(v / 32.0), 64.0) / 63.0, floor(v / 2048.0) / 31.0, 1.0);
#elif COLFMT == 5
    float v = a_color.x;
    vec4 color = vec4(mod(v, 32.0) / 31.0, mod(floor(v / 32.0), 32.0) / 31.0, mod(floor(v / 1024.0), 32.0) / 31.0,
                      floor(v / 32768.0));
#elif COLFMT == 6
    float v = a_color.x;
    vec4 color = vec4(mod(v, 16.0), mod(floor(v / 16.0), 16.0), mod(floor(v / 256.0), 16.0), floor(v / 4096.0)) / 15.0;
#else
    vec4 color = a_color;
#endif
#if THROUGH
    if (u_texgen.z > 0.5 && position.z < 0.0) position.z += 65536.0;
    vec3 screen = position;
    float w = 1.0;
#else
    vec4 view = u_world_view * vec4(position, 1.0);
    vec4 clip = u_projection * view;
    float w = clip.w;
    vec3 ndc = clip.xyz / clip.w;
    vec3 screen = vec3(ndc.xy * u_viewport.xy + u_viewport.zw, ndc.z * u_depth.x + u_depth.y);
#if FOG
    v_fog = clamp((view.z + u_fog.x) * u_fog.y, 0.0, 1.0);
#endif
#endif
    gl_Position = vec4((screen.x / u_target.x * 2.0 - 1.0) * w, (1.0 - screen.y / u_target.y * 2.0) * w,
                       (screen.z / 65535.0 * 2.0 - 1.0) * w, w);
#if LIT
    color = light(color, (u_world * vec4(position, 1.0)).xyz, unit((u_world * vec4(normal, 0.0)).xyz));
#endif
    v_color = color;
#if TEXTURED
    vec2 texcoord = a_texcoord * u_format.y;
#if TEXGEN == 3
    vec2 t = texcoord * u_uvscale.xy;
#elif TEXGEN == 2
    vec3 n = unit((u_world * vec4(normal, 0.0)).xyz);
    vec2 t = vec2((1.0 + dot(n, u_shade[0].xyz)) * 0.5, (1.0 + dot(n, u_shade[1].xyz)) * 0.5);
#elif TEXGEN == 1
#if SOURCE == 3
    vec3 s = normal;
#elif SOURCE == 2
    vec3 s = unit(normal);
#elif SOURCE == 1
    vec3 s = vec3(texcoord, 0.0);
#else
    vec3 s = position;
#endif
    v_texcoord = (u_texmatrix * vec4(s, 1.0)).xyz;
#else
    vec2 t = texcoord * u_uvscale.xy + u_uvscale.zw;
#endif
#if !PROJECTIVE
    v_texcoord = t * u_texmap.xy + u_texmap.zw;
#endif
#endif
}
)";

const char *kVariantFragmentShader = R"(
uniform sampler2D u_texture;
uniform vec4 u_texmap;
uniform vec4 u_envcolor;
uniform vec4 u_alpha;
uniform vec4 u_fogcolor;
uniform vec4 u_blendfix;
uniform vec4 u_alphaout;
uniform vec4 u_colortest;
uniform vec4 u_colormask;
#if TEXTURED
#if PROJECTIVE
varying vec3 v_texcoord;
#else
varying vec2 v_texcoord;
#endif
#endif
varying vec4 v_color;
#if FOG
varying float v_fog;
#endif
void main() {
    vec4 c = v_color;
#if TEXTURED
#if PROJECTIVE
    vec4 t = texture2D(u_texture, v_texcoord.xy / v_texcoord.z * u_texmap.xy + u_texmap.zw);
#else
    vec4 t = texture2D(u_texture, v_texcoord);
#endif
#if TCC
    float ta = t.a;
#else
    float ta = 1.0;
#endif
#if TEXFUNC == 0
    c = vec4(c.rgb * t.rgb, c.a * ta);
#elif TEXFUNC == 1
#if TCC
    c = vec4(mix(c.rgb, t.rgb, t.a), c.a);
#else
    c = vec4(t.rgb, c.a);
#endif
#elif TEXFUNC == 2
    c = vec4(mix(c.rgb, u_envcolor.rgb, t.rgb), c.a * ta);
#elif TEXFUNC == 3
#if TCC
    c = t;
#else
    c = vec4(t.rgb, c.a);
#endif
#else
    c = vec4(min(c.rgb + t.rgb, 1.0), c.a * ta);
#endif
#if DOUBLE
    c.rgb = min(c.rgb * 2.0, 1.0);
#endif
#endif
#if ATFAST
    // floor(a * 255 + 0.5) > r  <=>  a * 255 >= r + 0.5 (GREATER);
    // >= r  <=>  a * 255 >= r - 0.5 (GEQUAL); u_alpha.w holds that bound.
    if (c.a * 255.0 < u_alpha.w) discard;
#endif
#if FOG
    c.rgb = mix(u_fogcolor.rgb, c.rgb, v_fog);
#endif
#if CTEST
    bool equal_rgb = all(equal(floor(c.rgb * 255.0 + 0.5) * u_colormask.rgb, u_colortest.rgb * u_colormask.rgb));
#if CTFUNC == 0
    discard;
#elif CTFUNC == 2
    if (!equal_rgb) discard;
#else
    if (equal_rgb) discard;
#endif
#endif
#if ATEST && !ATFAST
    float a = floor(c.a * 255.0 + 0.5);
    float r = u_alpha.y;
#if ATFUNC == 0
    bool fail = true;
#elif ATFUNC == 2
    bool fail = a != r;
#elif ATFUNC == 3
    bool fail = a == r;
#elif ATFUNC == 4
    bool fail = a >= r;
#elif ATFUNC == 5
    bool fail = a > r;
#elif ATFUNC == 6
    bool fail = a <= r;
#else
    bool fail = a < r;
#endif
#if ATBLEND
    // Opaque draw: blending with a 0/1 alpha gives the alpha test's colours
    // without a discard (a punch-through object stalls the Vita's GPU).
    c.a = fail ? 0.0 : 1.0;
#else
    if (fail) discard;
#endif
#endif
#if BLENDFIX
    c.rgb *= u_blendfix.rgb;
#endif
#if ALPHAOUT
    c.a = u_alphaout.y;
#endif
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
#if SHARPEN
    // Unsharp mask against the four neighbouring texels: the picture keeps
    // its edges as it is enlarged to the screen.
    vec3 c = texture2D(u_texture, v_texcoord).rgb;
    vec3 n = texture2D(u_texture, v_texcoord + vec2(TEXEL_X, 0.0)).rgb + texture2D(u_texture, v_texcoord - vec2(TEXEL_X, 0.0)).rgb +
             texture2D(u_texture, v_texcoord + vec2(0.0, TEXEL_Y)).rgb + texture2D(u_texture, v_texcoord - vec2(0.0, TEXEL_Y)).rgb;
    gl_FragColor = vec4(clamp(c + STRENGTH * (c - 0.25 * n), 0.0, 1.0), 1.0);
#else
    gl_FragColor = vec4(texture2D(u_texture, v_texcoord).rgb, 1.0);
#endif
}
)";

// ---------------------------------------------------------------- state

struct RenderTarget {
    GLuint framebuffer{}, texture{}, depth{};
    int width{}, height{};
    bool dynamic{};  // screen-sized, drawn at the dynamic resolution
    int draw_q{};    // eighths it was last drawn at
    GLint sampling[4]{-1, -1, -1, -1};
};

struct Texture {
    GLuint name{};
    std::uint64_t hash{}, probe{};
    std::uint64_t checked_frame{UINT64_MAX};
    std::uint32_t address{}, bytes{};
    bool stale{true};  // VRAM texture: a block transfer may have rewritten it
    bool mipmapped{};  // uploaded with a generated mip chain
    std::uint8_t min_alpha{255u};  // lowest texel alpha of the upload
    std::uint32_t upload_mode{};   // g_probe_mode it was uploaded in
    bool foliage{};                // cut-out and mostly green (trees, bushes)
    float clear{};                 // fraction of texels with alpha <= 16
    GLint sampling[4]{-1, -1, -1, -1};
};

// Uniforms are recorded by slot; the GL thread keeps the latest value of each
// and uploads to whichever program variant draws whatever it has not seen.
enum Slot : GLint {
    kWorld, kWorldView, kProjection, kViewport, kDepth, kTarget, kFog, kFormat, kTexgen, kUvScale, kTexMatrix,
    kShade, kLighting, kTexMap, kTexEnv, kEnvColor, kAlpha, kFogColor, kBlendFix, kAlphaOut, kColorTest, kColorMask,
    kSlotCount
};
constexpr const char *kSlotNames[kSlotCount]{
    "u_world", "u_world_view", "u_projection", "u_viewport", "u_depth", "u_target", "u_fog",
    "u_format", "u_texgen", "u_uvscale", "u_texmatrix", "u_shade", "u_lighting", "u_texmap",
    "u_texenv", "u_envcolor", "u_alpha", "u_fogcolor", "u_blendfix", "u_alphaout", "u_colortest", "u_colormask"};

struct Program {
    GLuint id{};
    GLint texture{};
    std::array<GLint, kSlotCount> slots{};             // locations, -1 when the variant does not use it
    std::array<std::uint32_t, kSlotCount> versions{};  // slot value versions last uploaded (GL thread)
};

std::array<std::uint32_t, 256> g_commands{};
std::array<float, 12> g_world{}, g_view{}, g_texgen{};
bool g_batch_world{};  // the pending batch's positions are in world space
std::array<float, 16> g_projection{};
std::uint32_t g_world_cursor{}, g_view_cursor{}, g_projection_cursor{}, g_texgen_cursor{};
std::uint32_t g_vertex_address{}, g_index_address{};
std::array<std::uint8_t, 1024> g_clut{};   // palette cached by LOAD_CLUT, as on the GE
std::uint64_t g_clut_hash{};
// Pending batch: vertices in the PSP component formats (see BatchLayout),
// all of one vertex type.
// Scratch buffers that are always written after they grow: no zero fill.
template <typename T> struct UninitAllocator : std::allocator<T> {
    template <typename U> struct rebind { using other = UninitAllocator<U>; };
    UninitAllocator() = default;
    template <typename U> UninitAllocator(const UninitAllocator<U> &) noexcept {}
    template <typename U> void construct(U *p) noexcept { ::new (static_cast<void *>(p)) U; }
    template <typename U, typename... A> void construct(U *p, A &&...args) { ::new (static_cast<void *>(p)) U(std::forward<A>(args)...); }
};
template <typename T> using ScratchVector = std::vector<T, UninitAllocator<T>>;

ScratchVector<std::uint8_t> g_batch;
std::uint32_t g_batch_count{}, g_batch_vtype{};
// Lowest alpha the batch's vertices (or the material) can give, 0-255: with
// the bound texture's lowest alpha it tells when the alpha test cannot fail,
// and the shader then needs no discard (which on the Vita's PowerVR GPU
// turns off hidden surface removal: ~38 ms of GPU time a frame in a race).
std::uint32_t g_batch_min_alpha{255u};
std::uint32_t g_texture_min_alpha{255u};  // the texture bind_texture last bound
bool g_keep_discard{std::getenv("PSPRECOMP_BURNOUT_KEEP_DISCARD") != nullptr};
// Non-null: the batch vertices are read in place from the snapshot instead of
// g_batch (PSP layout already fits, see BatchLayout).
const std::uint8_t *g_batch_in_place{};
ScratchVector<std::uint16_t> g_batch_indices;  // explicit indices (indexed, flat and sprite PRIMs)
// The batch's triangles as runs of two words: first vertex (or, kind 7, first
// explicit index) and count | kind << 24, kind being the PRIM type for lists,
// strips and fans of consecutive vertices. The GL thread expands them.
std::vector<std::uint32_t> g_batch_runs;
std::uint32_t g_batch_index_total{};
bool g_lighting_dirty{true};  // a lighting or material command changed since the last upload
bool g_drawing{true};         // false while a frame is skipped
std::unordered_map<std::uint32_t, RenderTarget> g_targets;
std::unordered_map<std::uint64_t, Texture> g_textures;
Program g_uber, g_present, g_present_sharp;  // GL thread (sharp: enlarged pictures)
#if defined(__vita__)
GLuint g_vertex_buffer{}, g_index_buffer{};  // wrap per-draw scratch memory (vglBufferData)
#endif
Stats g_stats;
psprecomp::GuestMemory *g_memory{};

// What the GPU is asked to do, for the "gpu:" trace line every 300 frames
// (decode thread). Each framebuffer switch ends a GXM scene, which on the
// tiled SGX stores the target and later reloads it.
struct TargetUse {
    std::uint64_t draws{}, binds{}, sampled{}, blended{}, through{};
    int width{};
};
struct GpuCounters {
    std::uint64_t switches{}, draws{}, blended{}, discards{}, discards_dropped{}, alpha_blended{}, through{}, samples{}, textured{}, mipmapped{}, minmip{}, skinned{}, weights{}, copied{}, skin_calls{}, skin_hits{};
    double skin_ms{};
    std::unordered_map<std::uint32_t, TargetUse> targets;  // by target key
} g_gpu;
// Scale of each render target as created (decode thread): the screen-sized
// ones at the render scale, narrower ones optionally at 1x (offscreen_1x).
bool g_offscreen_1x{};
// Dynamic resolution (decode thread): the screen-sized targets are allocated
// at the render scale and drawn at g_dynamic_q eighths, lowered while the GPU
// makes the GL thread wait at present, raised again once it no longer does.
bool g_dynamic_resolution{std::getenv("PSPRECOMP_BURNOUT_DYNAMIC_RES") != nullptr};
int g_dynamic_min{12};
int g_dynamic_q{0};  // 0 until the first frame: the render scale
bool g_stencil_ab{};
// GPU probe (diagnostic): every 600 frames the alpha-tested textured draws
// change, so one log tells what their GPU time depends on: 0 normal, 1 the
// original alpha test (normal is the fast one), 2 nearest filtering, 3 each drawn as two draws, 4 no
// depth writes, 5 cut-outs merged (world applied as decoded). (gl-64 probed scissor 1/16, no discard, untextured; gl-66
// texture formats; gl-67 alpha test as blending.)
bool g_gpu_probe{};
// Test F: deferred cut-outs (drawn after the opaque scenery) blend with a 0/1 alpha instead of discarding.
bool g_blend_deferred_cutouts{std::getenv("PSPRECOMP_BURNOUT_BLEND_DEFERRED") != nullptr};
// Test: cut-out draws (alpha-tested, not blended: trees, hedges, fences) are not drawn.
bool g_no_cutouts{std::getenv("PSPRECOMP_BURNOUT_NO_CUTOUTS") != nullptr};
bool g_draw_is_cutout{};
bool g_texture_foliage{};
// Far foliage thinning (gl-77): foliage PRIMs (cut-out, green textures in
// VRAM) farther than g_thin_near from the camera are partly left out, more
// of them past g_thin_far, chosen by vertex address so the same trees stay.
bool g_thin_foliage{std::getenv("PSPRECOMP_BURNOUT_THIN_FOLIAGE") != nullptr};
float g_thin_near{std::getenv("PSPRECOMP_BURNOUT_THIN_NEAR") != nullptr ? static_cast<float>(std::atof(std::getenv("PSPRECOMP_BURNOUT_THIN_NEAR"))) : 120.0f};
float g_thin_far{std::getenv("PSPRECOMP_BURNOUT_THIN_FAR") != nullptr ? static_cast<float>(std::atof(std::getenv("PSPRECOMP_BURNOUT_THIN_FAR"))) : 240.0f};
std::unordered_set<std::uint32_t> g_foliage_addresses;  // decode thread
std::uint64_t g_thinned_prims{}, g_thinned_vertices{};
float g_texture_clear{};  // the texture bind_texture last bound is foliage
// Cut-out textures not drawn (trees): guest addresses; PC: PSPRECOMP_BURNOUT_SKIP_TEX=a,b,...
std::vector<std::uint32_t> g_skipped_cutouts = [] {
    std::vector<std::uint32_t> list;
    if (const char *v = std::getenv("PSPRECOMP_BURNOUT_SKIP_TEX")) {
        for (const char *at = v; *at != 0;) {
            char *end = nullptr;
            list.push_back(static_cast<std::uint32_t>(std::strtoul(at, &end, 16)) & 0x0FFFFFFFu);
            if (end == at) break;
            at = *end == ',' ? end + 1 : end;
        }
    }
    return list;
}();
// Opaque alpha-tested draws as 0/1-alpha blending (no discard).
bool g_alpha_blend_test{std::getenv("PSPRECOMP_BURNOUT_ALPHA_BLEND_TEST") != nullptr};
std::atomic<std::uint32_t> g_probe_mode{0u};
// The alpha test as one compare of the unrounded alpha, before fog (exact
// for GREATER / GEQUAL; PC captures identical but 2 pixels off by one in green).
bool g_fast_alpha_test{std::getenv("PSPRECOMP_BURNOUT_SLOW_ALPHA_TEST") == nullptr};
std::uint32_t probe_key(std::uint32_t key) noexcept {
    // Probe mode 1: the original alpha test.
    const std::uint32_t func = key >> 18u & 7u;
    const bool fast = g_fast_alpha_test != (g_probe_mode.load(std::memory_order_relaxed) == 1u);
    if (fast && (key >> 17u & 1u) != 0u && (key >> 26u & 1u) == 0u && (func == 6u || func == 7u)) key |= 1u << 27u;
    return key;
}
bool g_mipmaps{std::getenv("PSPRECOMP_BURNOUT_NO_MIPMAPS") == nullptr};
// Stencil A/B diagnostic: 0 full, 1 minimal, 2 off, rotating every 600 frames.
std::uint32_t stencil_mode() noexcept { return g_stencil_ab ? static_cast<std::uint32_t>((g_stats.frames / 600u) % 3u) : 0u; }

std::uint32_t cmd(std::uint32_t index) noexcept { return g_commands[index] & 0x00FFFFFFu; }
float float24(std::uint32_t data) noexcept { return std::bit_cast<float>(data << 8u); }

// Test: variant fragment shaders at medium precision (half floats on the SGX).
bool g_fragment_mediump{};

GLuint compile(GLenum type, const std::string &defines, const char *source, std::string &error) {
#if defined(__vita__)
    const bool mediump = g_fragment_mediump && type == GL_FRAGMENT_SHADER && !defines.empty();
    const std::string text = (mediump ? std::string("precision mediump float;\n") : std::string()) + defines + source;
#else
    const std::string text = "#version 120\n" + defines + source;
#endif
    const char *sources[1]{text.c_str()};
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, sources, nullptr);
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

std::string g_shader_cache;  // directory, empty: no cache
bool g_cached_memory{};

void bind_attributes(GLuint id, bool present) {
    glBindAttribLocation(id, 0, "a_position");
    glBindAttribLocation(id, 1, "a_texcoord");
    if (!present) {
        glBindAttribLocation(id, 2, "a_color");
        glBindAttribLocation(id, 3, "a_normal");
    }
}

bool find_uniforms(Program &program, bool present, std::string &error) {
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
    for (int i = 0; i < kSlotCount; ++i) program.slots[i] = u(kSlotNames[i]);
    return true;
}

#if defined(__vita__)
// Program binaries (vitaGL's glGetProgramBinary format) named by a hash of
// the sources and defines, so a changed shader never loads a stale one.
// vitaGL does not bound the copy, hence the large buffer.
constexpr std::size_t kProgramBinaryMax = 1u << 20u;

std::string cache_path(const char *vertex, const char *fragment, const std::string &defines) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    // The build date stands for the vitaGL and shader compiler it was linked
    // with: a newer build never loads an older binary.
    for (const std::string &part : {std::string(vertex), std::string(fragment), defines, std::string(__DATE__ __TIME__)})
        for (const char c : part) hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ull;
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llx.bin", static_cast<unsigned long long>(hash));
    return g_shader_cache + name;
}

bool load_cached(Program &program, const std::string &path, bool present) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    std::vector<std::uint8_t> binary;
    std::uint8_t chunk[16384];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0u;) binary.insert(binary.end(), chunk, chunk + n);
    std::fclose(file);
    if (binary.size() < 64u) return false;
    program.id = glCreateProgram();
    bind_attributes(program.id, present);
    glProgramBinary(program.id, 0, binary.data(), static_cast<GLsizei>(binary.size()));
    std::string error;
    if (find_uniforms(program, present, error)) return true;
    trace(("cached program unusable, compiling it again: " + error).c_str());
    glDeleteProgram(program.id);
    program = {};
    std::remove(path.c_str());
    return false;
}

void store_cached(const Program &program, const std::string &path) {
    static std::vector<std::uint8_t> binary(kProgramBinaryMax);
    GLsizei length = 0;
    GLenum format = 0;
    glGetProgramBinary(program.id, static_cast<GLsizei>(binary.size()), &length, &format, binary.data());
    if (length <= 0 || static_cast<std::size_t>(length) > binary.size()) return;
    const std::string partial = path + ".part";
    std::FILE *file = std::fopen(partial.c_str(), "wb");
    if (file == nullptr) return;
    const bool written = std::fwrite(binary.data(), 1, static_cast<std::size_t>(length), file) == static_cast<std::size_t>(length);
    if (std::fclose(file) == 0 && written) std::rename(partial.c_str(), path.c_str());
    else std::remove(partial.c_str());
}
#endif

bool link(Program &program, const char *vertex, const char *fragment, bool present, std::string &error,
          const std::string &defines = {}) {
#if defined(__vita__)
    const std::string cached = g_shader_cache.empty() ? std::string() : cache_path(vertex, fragment, defines);
    if (!cached.empty() && load_cached(program, cached, present)) return true;
#endif
    const GLuint vs = compile(GL_VERTEX_SHADER, defines, vertex, error);
    if (vs == 0u) return false;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, defines, fragment, error);
    if (fs == 0u) return false;
    program.id = glCreateProgram();
    glAttachShader(program.id, vs);
    glAttachShader(program.id, fs);
    bind_attributes(program.id, present);
    glLinkProgram(program.id);
    if (!find_uniforms(program, present, error)) return false;
#if defined(__vita__)
    if (!cached.empty()) store_cached(program, cached);
#endif
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
    UseProgram, BindFramebuffer, BindTexture, TexParameter, TexImage, TexImageP8, Uniform4, UniformMatrix, Draw, CreateTarget,
    Present, Capture, StencilFunc, StencilOp, StencilMask, Prepass
};

// Cut-out depth prepass (gl-72): an opaque alpha-tested textured draw that
// writes depth is drawn twice: first depth only, with the alpha test's
// discard, then in colour without discard where the depth is EQUAL. On the
// Vita's PowerVR the costly punch-through pass then writes no colour, and the
// colour pass gets hidden surface removal back.
bool g_cutout_prepass{std::getenv("PSPRECOMP_BURNOUT_PREPASS") != nullptr};
bool g_prepass_eligible{};  // decode thread: the draw being recorded qualifies
std::uint64_t g_prepassed{};  // decode thread: such draws, for the gpu line

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

// Cut-outs after the opaque scenery (reorder test): an alpha-tested opaque
// draw is recorded, with all its state, into g_deferred_ops, which joins the
// frame at the next draw that cannot move ahead of it (blending, no depth
// test, stencil, another target...). The opaque draws in between then reach
// the GPU first, so the punch-through ones behind them are rejected early.
bool g_reorder_cutouts{std::getenv("PSPRECOMP_BURNOUT_REORDER") != nullptr};
bool g_recording_deferred{};
std::vector<std::uint32_t> g_deferred_ops;
std::vector<GLint *> g_deferred_sampling;  // sampler caches the deferred ops set
GLuint g_deferred_target{};
std::uint64_t g_deferred_draws{}, g_deferred_splices{};

void record(Op op, std::initializer_list<std::uint32_t> args = {}) {
    std::vector<std::uint32_t> &ops = g_recording_deferred ? g_deferred_ops : g_frame.ops;
    ops.push_back(static_cast<std::uint32_t>(op));
    ops.insert(ops.end(), args.begin(), args.end());
}

std::uint32_t record_data(const void *bytes, std::size_t size) {
    // Appended rather than resized: no zero fill before the copy.
    std::vector<std::uint8_t> &data = g_frame.data;
    static constexpr std::uint8_t kPadding[8]{};
    data.insert(data.end(), kPadding, kPadding + ((8u - (data.size() & 7u)) & 7u));
    const std::size_t at = data.size();
    const auto *first = static_cast<const std::uint8_t *>(bytes);
    data.insert(data.end(), first, first + size);
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
    int stencil_test{-1};
    std::uint32_t stencil_func[3]{~0u, ~0u, ~0u}, stencil_op[3]{~0u, ~0u, ~0u}, stencil_mask{~0u};
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

void set_stencil(GLenum func, std::uint32_t ref, std::uint32_t mask, GLenum fail, GLenum depth_fail, GLenum pass,
                 std::uint32_t write_mask) {
    const std::uint32_t f[3]{func, ref, mask}, o[3]{fail, depth_fail, pass};
    if (std::memcmp(f, g_state.stencil_func, sizeof(f)) != 0) {
        std::memcpy(g_state.stencil_func, f, sizeof(f));
        record(Op::StencilFunc, {func, ref, mask});
    }
    if (std::memcmp(o, g_state.stencil_op, sizeof(o)) != 0) {
        std::memcpy(g_state.stencil_op, o, sizeof(o));
        record(Op::StencilOp, {fail, depth_fail, pass});
    }
    if (g_state.stencil_mask != write_mask) {
        g_state.stencil_mask = write_mask;
        record(Op::StencilMask, {write_mask});
    }
}

GLenum stencil_op(std::uint32_t op) noexcept {
    static constexpr GLenum table[8]{GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR, GL_KEEP, GL_KEEP};
    return table[op & 7u];
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
void set_sampling(GLint (&current)[4], bool linear, bool clamp_u, bool clamp_v, GLint min_filter = 0) {
    const GLint wanted[4]{min_filter != 0 ? min_filter : linear ? GL_LINEAR : GL_NEAREST, linear ? GL_LINEAR : GL_NEAREST,
                          clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT, clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT};
    static constexpr GLenum names[4]{GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T};
    if (g_recording_deferred) {
        // Replayed after draws recorded later: set every parameter, and the
        // cache is forgotten when the deferred ops join the frame.
        for (int i = 0; i < 4; ++i) current[i] = -1;
        g_deferred_sampling.push_back(current);
    }
    for (int i = 0; i < 4; ++i)
        if (current[i] != wanted[i]) {
            current[i] = wanted[i];
            record(Op::TexParameter, {names[i], static_cast<std::uint32_t>(wanted[i])});
        }
}

// ---------------------------------------------------------------- render targets

std::uint32_t target_key(std::uint32_t address) noexcept { return address & 0x001FFFF0u; }
extern int g_scale;

RenderTarget &render_target(std::uint32_t key, std::uint32_t stride) {
    auto [it, inserted] = g_targets.try_emplace(key);
    RenderTarget &target = it->second;
    if (!inserted) return target;
    target.width = static_cast<int>(std::max<std::uint32_t>(stride, 64u));
    target.height = 272;
    target.texture = g_next_texture_id++;
    target.framebuffer = g_next_framebuffer_id++;
    // Screen-sized targets (the frame buffers) at the render scale; narrower
    // ones are the game's effect buffers, which offscreen_1x keeps at 1x.
    const int scale = g_offscreen_1x && target.width < 480 ? 8 : g_scale;
    target.dynamic = g_dynamic_resolution && target.width >= 480;
    target.draw_q = scale;
    record(Op::CreateTarget, {target.framebuffer, target.texture, static_cast<std::uint32_t>(target.width),
                              static_cast<std::uint32_t>(target.height), static_cast<std::uint32_t>(scale)});
    char line[96];
    std::snprintf(line, sizeof(line), "render target %06X: %dx%d at x%g", key, target.width, target.height, scale / 8.0);
    trace(line);
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

// Hash of one 16-byte block in every 128 (a quarter of the cache lines): the
// per-frame check of a texture in RAM, the full hash only following a change.
std::uint64_t probe_hash(const std::uint8_t *data, std::size_t size) noexcept {
    std::uint32_t a = 0x811C9DC5u, b = 0x01000193u;
    for (std::size_t i = 0; i + 16u <= size; i += 128u) {
        std::uint32_t w[4];
        std::memcpy(w, data + i, 16u);
        a = (a ^ w[0] ^ w[2]) * 0x01000193u;
        b = (b ^ w[1] ^ w[3]) * 0x85EBCA6Bu;
    }
    return (static_cast<std::uint64_t>(a) << 32u) | b;
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
            ++g_gpu.samples;
            ++g_gpu.targets[target_key(address)].sampled;
            bind_texture_object(it->second.texture);
            set_sampling(it->second.sampling, linear, true, true);
            return {static_cast<float>(width) / static_cast<float>(it->second.width),
                    -static_cast<float>(height) / static_cast<float>(it->second.height), 0.0f, 1.0f};
        }
    }

    ++g_gpu.textured;
    const std::uint32_t levels = (cmd(0xC2u) >> 16u) & 7u;
    if (levels != 0u) ++g_gpu.mipmapped;
    if ((cmd(0xC6u) & 7u) >= 4u) ++g_gpu.minmip;
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
    static const bool recheck_vram = std::getenv("PSPRECOMP_BURNOUT_VRAM_RECHECK") != nullptr;
    const bool in_vram = (address & 0x0F000000u) == 0x04000000u && !recheck_vram;
    const std::uint32_t upload_mode = 0u;  // gl-66 probe: 1 P8 paletted, 2 no mips (no GPU difference)
    const bool reformat = !fresh && texture.upload_mode != upload_mode;
    if ((reformat || (in_vram ? texture.stale : texture.checked_frame != g_stats.frames)) && source != nullptr) {
        texture.checked_frame = g_stats.frames;
        texture.stale = false;
        // Every 64 frames (staggered) the whole texture is hashed anyway.
        const std::uint64_t probe = probe_hash(source, bytes);
        const bool full = fresh || reformat || probe != texture.probe || ((g_stats.frames + texture.name) & 63u) == 0u;
        texture.probe = probe;
        if (full) g_stats.texture_bytes_hashed += bytes;
        const std::uint64_t hash = full ? fnv(source, bytes) : texture.hash;
        if (fresh || reformat || hash != texture.hash) {
            texture.hash = hash;
            texture.upload_mode = upload_mode;
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
            if (const char *dump = std::getenv("PSPRECOMP_BURNOUT_TEX_DUMP");
                dump != nullptr && (address & 0x0FFFFFFFu) == std::strtoul(dump, nullptr, 16)) {
                char name[96];
                std::snprintf(name, sizeof(name), "tex_%08X_f%llu_%ux%u.rgba", address, static_cast<unsigned long long>(g_stats.frames),
                              width, height);
                if (std::FILE *file = std::fopen(name, "wb")) {
                    std::fwrite(rgba.data(), 1u, rgba.size(), file);
                    std::fclose(file);
                }
            }
            // The game's textures carry mip levels (most of the scenery's):
            // the GPU builds the chain from level 0 instead. Without one a
            // distant road samples a 256-texel texture sparsely, which on the
            // SGX misses the texture cache on nearly every fetch.
            texture.mipmapped = g_mipmaps && upload_mode == 0u && levels != 0u && width >= 8u && height >= 8u;
            std::uint8_t min_alpha = 255u;
            for (std::size_t i = 3u; i < rgba.size(); i += 4u) min_alpha = std::min(min_alpha, rgba[i]);
            texture.min_alpha = min_alpha;
            {
                // Foliage (trees, bushes): a cut-out texture whose visible
                // texels are mostly green. The "no trees" option skips those.
                std::size_t clear = 0u;
                std::uint64_t sum[3]{};
                for (std::size_t i = 0u; i < rgba.size(); i += 4u) {
                    if (rgba[i + 3u] <= 16u) { ++clear; continue; }
                    sum[0] += rgba[i]; sum[1] += rgba[i + 1u]; sum[2] += rgba[i + 2u];
                }
                const std::size_t texels = static_cast<std::size_t>(width) * height;
                const double clear_fraction = static_cast<double>(clear) / static_cast<double>(texels);
                const double opaque = static_cast<double>(std::max<std::size_t>(texels - clear, 1u));
                const double r = static_cast<double>(sum[0]) / opaque, g = static_cast<double>(sum[1]) / opaque,
                             b = static_cast<double>(sum[2]) / opaque;
                texture.clear = static_cast<float>(clear_fraction);
                texture.foliage = (address & 0x0F000000u) == 0x04000000u && clear_fraction >= 0.05 && g >= b * 1.4 && r <= g * 1.25 && g < 200.0;
                if (texture.foliage) g_foliage_addresses.insert(address);
                else g_foliage_addresses.erase(address);
                if (std::getenv("PSPRECOMP_BURNOUT_ATEST_STATS") != nullptr)
                    std::fprintf(stderr, "[texfmt] fmt %u clutfmt %u %ux%u levels %u swz %d minA %u clear %.3f rgb %.0f %.0f %.0f foliage %d addr %08X\n",
                                 format, cmd(0xC5u) & 3u, width, height, levels, swizzled ? 1 : 0, min_alpha, clear_fraction, r, g, b,
                                 texture.foliage ? 1 : 0, address);
            }
            if (upload_mode == 1u && (format == 4u || format == 5u)) {
                // Indices as stored (one byte each) and the 256-entry palette.
                std::vector<std::uint8_t> indices(static_cast<std::size_t>(width) * height);
                for (std::uint32_t y = 0; y < height; ++y) {
                    const std::uint8_t *row = data + y * row_bytes;
                    for (std::uint32_t x = 0; x < width; ++x)
                        indices[static_cast<std::size_t>(y) * width + x] =
                            format == 5u ? row[x] : static_cast<std::uint8_t>((row[x / 2u] >> ((x & 1u) * 4u)) & 15u);
                }
                std::vector<std::uint8_t> palette(1024u);
                for (std::uint32_t i = 0; i < 256u; ++i) {
                    const auto c = clut_color(i);
                    std::memcpy(&palette[i * 4u], c.data(), 4u);
                }
                record(Op::TexImageP8, {width, height, static_cast<std::uint32_t>(g_frame.blobs.size())});
                g_frame.blobs.push_back(std::move(indices));
                g_frame.blobs.push_back(std::move(palette));
            } else {
                // Uploads stay in frame order (draws recorded later may use
                // the texture before a deferred draw is replayed).
                const bool deferred = g_recording_deferred;
                g_recording_deferred = false;
                if (deferred) record(Op::BindTexture, {texture.name});
                record(Op::TexImage, {width, height, static_cast<std::uint32_t>(g_frame.blobs.size()), texture.mipmapped ? 1u : 0u});
                g_recording_deferred = deferred;
                if (deferred) g_state.texture = ~0u;
                g_frame.blobs.push_back(std::move(rgba));
            }
            ++g_stats.texture_uploads;
        }
    }
    // PSP minification modes 4-7: nearest/linear texel, nearest/linear level.
    GLint min_filter = 0;
    const std::uint32_t min_mode = cmd(0xC6u) & 7u;
    if (texture.mipmapped && levels != 0u && min_mode >= 4u) {
        static constexpr GLint modes[4]{GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                        GL_LINEAR_MIPMAP_LINEAR};
        min_filter = modes[min_mode - 4u];
    }
    set_sampling(texture.sampling, linear, clamp_u, clamp_v, min_filter);
    g_texture_min_alpha = texture.min_alpha;
    g_texture_foliage = texture.foliage;
    g_texture_clear = texture.clear;
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

bool is_sun_box_draw(bool textured);  // below, with the batch layout

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
// Last value recorded for each uniform slot (locations are slots, see Slot).
struct CachedUniform {
    std::uint32_t count{};  // 0: never recorded
    std::array<float, 30 * 4> values{};
};
std::array<CachedUniform, kSlotCount> g_uniforms;

bool uniform_changed(GLint slot, const float *values, std::size_t count) {
    if (slot < 0 || slot >= kSlotCount) return false;
    CachedUniform &cached = g_uniforms[static_cast<std::size_t>(slot)];
    if (cached.count == count && std::memcmp(cached.values.data(), values, count * sizeof(float)) == 0) return false;
    cached.count = static_cast<std::uint32_t>(std::min(count, cached.values.size()));
    std::memcpy(cached.values.data(), values, cached.count * sizeof(float));
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
    static constexpr float kIdentity[12]{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    const float *world_rows = g_batch_world ? kIdentity : g_world.data();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            world[r * 4 + c] = world_rows[r * 3 + c];
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
    uniform_matrix(kWorld, world);
    uniform_matrix(kWorldView, world_view);
    uniform_matrix(kProjection, g_projection.data());
}

std::uint32_t upload_vertex_state(bool through);
void bind_vertices();

// After the GL state changed behind the caches' back (deferred ops).
void invalidate_record_caches() {
    // The render target stays: deferred ops only join the frame while their
    // target is still bound, and rebinding it would start a new GPU scene.
    const GLuint framebuffer = g_state.framebuffer;
    const int viewport_w = g_state.viewport_w, viewport_h = g_state.viewport_h;
    forget_state();
    g_state.framebuffer = framebuffer;
    g_state.viewport_w = viewport_w;
    g_state.viewport_h = viewport_h;
    for (auto &uniform : g_uniforms) uniform.count = 0u;
    g_lighting_dirty = true;
}

void splice_deferred() {
    if (g_deferred_ops.empty()) return;
    g_frame.ops.insert(g_frame.ops.end(), g_deferred_ops.begin(), g_deferred_ops.end());
    g_deferred_ops.clear();
    for (GLint *sampling : g_deferred_sampling) sampling[0] = sampling[1] = sampling[2] = sampling[3] = -1;
    g_deferred_sampling.clear();
    invalidate_record_caches();
    ++g_deferred_splices;
}

void flush() {
    if (g_batch_runs.empty() || !g_drawing) {
        g_batch_indices.clear();
        g_batch_runs.clear();
        g_batch_index_total = 0u;
        g_batch.clear();
        g_batch_count = 0u;
        g_batch_in_place = nullptr;
        return;
    }
    const std::uint32_t outer_phase = phase;
    phase = 6u;
    // A clock read costs about 1 us on the Vita and there are ~450 draws a
    // frame: one in 16 is timed and counts for 16.
    static std::uint32_t timed_draws = 0u;
    const bool timed = (timed_draws++ & 15u) == 0u;
    const double started = timed ? host_now_ms() : 0.0;
    const std::uint32_t address = cmd(0x9Cu);
    const std::uint32_t stride = cmd(0x9Du) & 0x7FCu;
    RenderTarget &target = render_target(target_key(address), stride);
    TargetUse &use = g_gpu.targets[target_key(address)];
    use.width = target.width;
    ++use.draws;
    ++g_gpu.draws;
    bool deferred_draw = false;
    if (g_reorder_cutouts) {
        // Depth-tested, depth-writing, unblended and unmasked: such draws
        // commute (but for exact depth ties). Alpha-tested textured ones
        // wait; anything else first lets the waiting ones through.
        const bool clear = (cmd(0xD3u) & 1u) != 0u;
        const std::uint32_t depth_func = cmd(0xDEu) & 7u;
        // A stencil that always passes and is only replaced where the depth
        // test passes ends up as the nearest draw's, in any order (the
        // scenery's: always, keep on depth fail, replace).
        const std::uint32_t stencil_ops = cmd(0xDDu);
        const bool stencil_by_depth = (cmd(0xDCu) & 7u) == 1u && ((stencil_ops >> 8u) & 7u) == 0u &&
                                      (((stencil_ops >> 16u) & 7u) == 0u || ((stencil_ops >> 16u) & 7u) == 2u);
        const bool plain = !clear && (g_batch_vtype & 0x800000u) == 0u && (cmd(0x21u) & 1u) == 0u &&
                           (cmd(0x23u) & 1u) != 0u && (cmd(0xE7u) & 1u) == 0u && depth_func >= 4u &&
                           ((cmd(0x24u) & 1u) == 0u || stencil_by_depth) && (cmd(0x28u) & 1u) == 0u && (cmd(0xE8u) & 0xFFFFFFu) == 0u &&
                           !((cmd(0x27u) & 1u) != 0u && (cmd(0xD8u) & 3u) != 1u);
        const bool cutout = plain && (cmd(0x1Eu) & 1u) != 0u && (cmd(0x22u) & 1u) != 0u && (cmd(0xDBu) & 7u) != 1u;
        static int debug_left = std::getenv("PSPRECOMP_BURNOUT_REORDER_DEBUG") != nullptr ? 40 : 0;
        static unsigned debug_seen = 0u;
        if (debug_left > 0 && (cmd(0x22u) & 1u) != 0u && (cmd(0xDBu) & 7u) != 1u && (cmd(0x1Eu) & 1u) != 0u && (++debug_seen % 997u) == 0u) {
            --debug_left;
            std::fprintf(stderr, "[reorder] clear %d through %d blend %u ztest %u zmask %06X zfunc %u stencil %u logic %u pmask %06X ctest %u/%u atest %06X sfunc %06X sop %06X\n",
                         clear ? 1 : 0, (g_batch_vtype & 0x800000u) != 0u ? 1 : 0, cmd(0x21u) & 1u, cmd(0x23u) & 1u, cmd(0xE7u) & 0xFFFFFFu,
                         depth_func, cmd(0x24u) & 1u, cmd(0x28u) & 1u, cmd(0xE8u) & 0xFFFFFFu, cmd(0x27u) & 1u, cmd(0xD8u) & 3u,
                         cmd(0xDBu) & 0xFFFFFFu, cmd(0xDCu) & 0xFFFFFFu, cmd(0xDDu) & 0xFFFFFFu);
        }
        if (!plain || (!g_deferred_ops.empty() && g_deferred_target != target.framebuffer)) splice_deferred();
        if (cutout) {
            invalidate_record_caches();
            g_recording_deferred = true;
            g_deferred_target = target.framebuffer;
            deferred_draw = true;
            ++g_deferred_draws;
        }
    }
    // A deferred draw's ops end here, whichever way flush returns.
    struct DeferredScope {
        bool active;
        ~DeferredScope() {
            if (!active) return;
            g_recording_deferred = false;
            invalidate_record_caches();
        }
    } deferred_scope{deferred_draw};
    const int frame_q = g_dynamic_q != 0 ? g_dynamic_q : g_scale;
    if (target.dynamic && target.draw_q != frame_q) {
        // Recreated at this frame's resolution (on the Vita's GPU a smaller
        // part of a large target costs as much as the whole of it), then
        // bound again with its viewport and scissor.
        target.draw_q = frame_q;
        record(Op::CreateTarget, {target.framebuffer, target.texture, static_cast<std::uint32_t>(target.width),
                                  static_cast<std::uint32_t>(target.height), static_cast<std::uint32_t>(frame_q)});
        g_state.framebuffer = ~0u;
        g_state.viewport_w = g_state.viewport_h = -1;
        g_state.scissor[0] = -1;
    }
    if (g_state.framebuffer != target.framebuffer) {
        g_state.framebuffer = target.framebuffer;
        record(Op::BindFramebuffer, {target.framebuffer, target.dynamic ? static_cast<std::uint32_t>(target.draw_q) : 0u});
        ++use.binds;
        ++g_gpu.switches;
    }
    if (g_state.viewport_w != target.width || g_state.viewport_h != target.height) {
        g_state.viewport_w = target.width;
        g_state.viewport_h = target.height;
        record(Op::Viewport, {static_cast<std::uint32_t>(target.width), static_cast<std::uint32_t>(target.height)});
    }
    const bool clear_mode = (cmd(0xD3u) & 1u) != 0u;
    const bool through = (g_batch_vtype & 0x800000u) != 0u;
    if (!through) upload_matrices();
    const std::uint32_t vertex_variant = upload_vertex_state(through);
    uniform4(kViewport, float24(cmd(0x42u)), float24(cmd(0x43u)),
             float24(cmd(0x45u)) - static_cast<float>(cmd(0x4Cu) & 0xFFFFu) / 16.0f,
             float24(cmd(0x46u)) - static_cast<float>(cmd(0x4Du) & 0xFFFFu) / 16.0f);
    const bool fog = !through && !clear_mode && (cmd(0x1Fu) & 1u) != 0u;
    uniform4(kDepth, float24(cmd(0x44u)), float24(cmd(0x47u)), through ? 1.0f : 0.0f, fog ? 1.0f : 0.0f);
    uniform4(kTarget, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 0.0f);
    uniform4(kFog, float24(cmd(0xCDu)), float24(cmd(0xCEu)), 0.0f, 0.0f);
    const std::uint32_t fog_color = cmd(0xCFu);
    uniform4(kFogColor, static_cast<float>(fog_color & 0xFFu) / 255.0f,
                static_cast<float>((fog_color >> 8u) & 0xFFu) / 255.0f, static_cast<float>((fog_color >> 16u) & 0xFFu) / 255.0f, 1.0f);

    const bool textured = !clear_mode && (cmd(0x1Eu) & 1u) != 0u;
    if (textured) {
        phase = 7u;
        const auto texmap = bind_texture(*g_memory);
        phase = 8u;
        uniform4(kTexMap, texmap[0], texmap[1], texmap[2], texmap[3]);
    }
    const std::uint32_t texfunc = cmd(0xC9u);
    uniform4(kTexEnv, textured ? 1.0f : 0.0f, static_cast<float>(std::min<std::uint32_t>(texfunc & 7u, 4u)),
                (texfunc & 0x100u) != 0u ? 1.0f : 0.0f, (texfunc & 0x10000u) != 0u ? 1.0f : 0.0f);
    const std::uint32_t env = cmd(0xCAu);
    uniform4(kEnvColor, static_cast<float>(env & 0xFFu) / 255.0f, static_cast<float>((env >> 8u) & 0xFFu) / 255.0f,
                static_cast<float>((env >> 16u) & 0xFFu) / 255.0f, 1.0f);
    const std::uint32_t alpha_test = cmd(0xDBu);
    const bool alpha_enabled = !clear_mode && (cmd(0x22u) & 1u) != 0u;
    // w: the bound of the fast alpha test (ATFAST), alpha * 255 against it.
    const float alpha_ref = static_cast<float>((alpha_test >> 8u) & 0xFFu);
    uniform4(kAlpha, static_cast<float>(alpha_test & 7u), alpha_ref, alpha_enabled ? 1.0f : 0.0f,
             (alpha_test & 7u) == 7u ? alpha_ref - 0.5f : alpha_ref + 0.5f);
    // Colour test: the sun flare's black quad (alpha only) is drawn with
    // "not equal to black", so on the PSP none of it lands.
    const std::uint32_t color_function = !clear_mode && (cmd(0x27u) & 1u) != 0u ? cmd(0xD8u) & 3u : 1u;
    if (color_function != 1u) {
        const std::uint32_t ref = cmd(0xD9u), mask = cmd(0xDAu);
        const auto channel = [](std::uint32_t value, int i) { return static_cast<float>((value >> (i * 8)) & 0xFFu); };
        uniform4(kColorTest, channel(ref, 0), channel(ref, 1), channel(ref, 2), static_cast<float>(color_function));
        uniform4(kColorMask, channel(mask, 0) != 0.0f ? 1.0f : 0.0f, channel(mask, 1) != 0.0f ? 1.0f : 0.0f,
                 channel(mask, 2) != 0.0f ? 1.0f : 0.0f, 0.0f);
    } else {
        uniform4(kColorTest, 0.0f, 0.0f, 0.0f, 1.0f);
    }

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

    bool blend_scaled = false, alpha_out = false;
    if (clear_mode) {
        const std::uint32_t clear = cmd(0xD3u);
        set_capability(GL_BLEND, g_state.blend, false);
        set_capability(GL_DEPTH_TEST, g_state.depth_test, true);
        set_depth(GL_ALWAYS, (clear & 0x400u) != 0u);
        set_color_mask((clear & 0x100u) != 0u, (clear & 0x200u) != 0u);
        // The alpha update clears the stencil too. To 0: the clear colour's
        // alpha is in the vertices, and the game only clears it to 0 so far.
        const bool stencil = (clear & 0x200u) != 0u && stencil_mode() == 0u;
        set_capability(GL_STENCIL_TEST, g_state.stencil_test, stencil);
        if (stencil) set_stencil(GL_ALWAYS, 0u, 0xFFu, GL_REPLACE, GL_REPLACE, GL_REPLACE, 0xFFu);
        set_capability(GL_CULL_FACE, g_state.cull, false);
        uniform4(kBlendFix, 1.0f, 1.0f, 1.0f, 0.0f);
        uniform4(kAlphaOut, 0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        const BlendShader blend = set_blend();
        uniform4(kBlendFix, blend.scale[0], blend.scale[1], blend.scale[2], blend.scaled ? 1.0f : 0.0f);
        uniform4(kAlphaOut, blend.alpha >= 0.0f ? 1.0f : 0.0f, std::max(blend.alpha, 0.0f), 0.0f, 0.0f);
        blend_scaled = blend.scaled;
        alpha_out = blend.alpha >= 0.0f;
        // Depth writes still happen with the test off.
        set_capability(GL_DEPTH_TEST, g_state.depth_test, true);
        set_depth((cmd(0x23u) & 1u) != 0u ? compare(cmd(0xDEu)) : GL_ALWAYS, (cmd(0xE7u) & 1u) == 0u);
        set_color_mask(true, true);
        if (is_sun_box_draw(textured)) set_color_mask(false, false);
        // Stencil test: on the PSP the stencil is the frame buffer's alpha,
        // written through the alpha write mask. Lakeside marks the lake's
        // pixels 2 and draws the sky's reflection where it equals 2; without
        // the test the reflection covered the lower half of the screen.
        const std::uint32_t func = cmd(0xDCu), op = cmd(0xDDu);
        bool stencil = (cmd(0x24u) & 1u) != 0u && stencil_mode() != 2u;
        if (stencil && stencil_mode() == 1u) {
            // Minimal (diagnostic): only draws that test the stencil or
            // write a non-zero value to it.
            const std::uint32_t ref = (func >> 8u) & (~cmd(0xE9u) & 0xFFu);
            const auto marks = [ref](std::uint32_t o) { return (o == 2u && ref != 0u) || o == 3u || o == 4u; };
            stencil = (func & 7u) != 1u || marks((op >> 8u) & 7u) || marks((op >> 16u) & 7u);
        }
        set_capability(GL_STENCIL_TEST, g_state.stencil_test, stencil);
        if (stencil) {
            set_stencil(compare(func), (func >> 8u) & 0xFFu, (func >> 16u) & 0xFFu, stencil_op(op), stencil_op(op >> 8u),
                        stencil_op(op >> 16u), ~cmd(0xE9u) & 0xFFu);
        }
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

    // Fragment half of the program variant; texture coordinate modes only
    // matter when textured, alpha test 1 (always) is no test.
    std::uint32_t variant = vertex_variant;
    if (textured) {
        variant |= 1u << 10u | std::min<std::uint32_t>(texfunc & 7u, 4u) << 11u | ((texfunc & 0x100u) != 0u ? 1u : 0u) << 14u |
                   ((texfunc & 0x10000u) != 0u ? 1u : 0u) << 15u | (((variant >> 2u) & 3u) == 1u ? 1u : 0u) << 16u;
    } else {
        variant &= ~0x3Cu;
    }
    if (fog) variant |= 1u << 9u;
    bool alpha_testing = alpha_enabled && (alpha_test & 7u) != 1u;
    if (alpha_testing && !g_keep_discard) {
        // The lowest alpha a fragment can have (filtering and mip levels stay
        // within the texels' range), less one for rounding.
        const std::uint32_t tex_alpha = textured && (texfunc & 0x100u) != 0u ? g_texture_min_alpha : 255u;
        const std::uint32_t func = texfunc & 7u;
        std::uint32_t lowest = g_batch_min_alpha;
        if (textured) lowest = func == 1u ? g_batch_min_alpha : func == 3u && (texfunc & 0x100u) != 0u ? tex_alpha
                                                                                                     : g_batch_min_alpha * tex_alpha / 255u;
        lowest = lowest == 0u ? 0u : lowest - 1u;
        const std::uint32_t ref = (alpha_test >> 8u) & 0xFFu;
        const std::uint32_t mask = (alpha_test >> 16u) & 0xFFu;
        // GREATER / GEQUAL / NOTEQUAL pass for every value above the reference
        // (only with a full mask: a partial one compares masked bits).
        const std::uint32_t test = alpha_test & 7u;
        if (mask == 0xFFu && ((test == 6u && lowest > ref) || (test == 7u && lowest >= ref) || (test == 3u && lowest > ref))) {
            alpha_testing = false;
            ++g_gpu.discards_dropped;
        }
    }
    if (alpha_testing) variant |= 1u << 17u | (alpha_test & 7u) << 18u;
    g_draw_is_cutout = alpha_testing && textured && !clear_mode && g_state.blend == 0;
    g_prepass_eligible = false;
    if (g_cutout_prepass && g_draw_is_cutout && !through) {
        // Depth tested and written, full colour mask, no logic op or colour
        // test, and a stencil that a second pass leaves as it is (always,
        // replaced or kept where the depth test passes).
        const std::uint32_t stencil_ops = cmd(0xDDu);
        const bool stencil_ok = (cmd(0x24u) & 1u) == 0u ||
                                ((cmd(0xDCu) & 7u) == 1u && ((stencil_ops >> 8u) & 7u) == 0u &&
                                 (((stencil_ops >> 16u) & 7u) == 0u || ((stencil_ops >> 16u) & 7u) == 2u));
        g_prepass_eligible = (cmd(0x23u) & 1u) != 0u && (cmd(0xE7u) & 1u) == 0u && (cmd(0xE8u) & 0xFFFFFFu) == 0u &&
                             (cmd(0x28u) & 1u) == 0u && color_function == 1u && stencil_ok && g_state.color_mask != 0;
    }
    if (alpha_testing && !clear_mode && g_state.blend == 0 && color_function == 1u &&
        (g_alpha_blend_test ||
         (g_blend_deferred_cutouts && g_recording_deferred))) {
        variant |= 1u << 26u;
        g_prepass_eligible = false;
        set_capability(GL_BLEND, g_state.blend, true);
        set_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        set_blend_equation(GL_FUNC_ADD);
        ++g_gpu.alpha_blended;
    }
    static const bool atest_stats = std::getenv("PSPRECOMP_BURNOUT_ATEST_STATS") != nullptr;
    if (atest_stats && alpha_testing) {
        static std::unordered_map<std::string, int> seen;
        static std::uint64_t calls = 0u;
        char key[160];
        std::snprintf(key, sizeof(key), "func %u ref %u mask %02X tex %d tcc %d texfunc %u texmin %u vmin %u blend %d zwrite %u vtype %06X tex %08X",
                      alpha_test & 7u, (alpha_test >> 8u) & 0xFFu, (alpha_test >> 16u) & 0xFFu, textured ? 1 : 0,
                      (texfunc & 0x100u) != 0u ? 1 : 0, texfunc & 7u, g_texture_min_alpha, g_batch_min_alpha, g_state.blend,
                      (cmd(0xE7u) & 0xFFFFu) == 0u ? 1u : 0u, g_batch_vtype,
                      (cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u));
        ++seen[key];
        if (++calls % 60000u == 0u) {
            std::vector<std::pair<int, std::string>> top;
            for (const auto &[k, v] : seen) top.push_back({v, k});
            std::sort(top.rbegin(), top.rend());
            for (std::size_t i = 0; i < top.size() && i < 40u; ++i) std::fprintf(stderr, "[atest] %6d %s\n", top[i].first, top[i].second.c_str());
            seen.clear();
        }
    }
    if (g_state.blend == 1) { ++g_gpu.blended; ++use.blended; }
    if (through) { ++g_gpu.through; ++use.through; }
    if (alpha_testing || color_function != 1u) ++g_gpu.discards;
    if (color_function != 1u) variant |= 1u << 23u | color_function << 24u;
    if (blend_scaled) variant |= 1u << 21u;
    if (alpha_out) variant |= 1u << 22u;
    if (g_state.program != variant) {
        g_state.program = variant;
        record(Op::UseProgram, {variant});
    }

    {
        // Bring-up: PSPRECOMP_BURNOUT_TARGET_TRACE=N lists frame N's draws by target.
        static const long trace_frame = [] {
            const char *v = std::getenv("PSPRECOMP_BURNOUT_TARGET_TRACE");
            return v != nullptr ? std::atol(v) : -1L;
        }();
        // PSPRECOMP_BURNOUT_TARGET_SKIP=A-B leaves out that frame's draws A to B.
        static std::uint64_t index_frame = ~0ull;
        static std::uint32_t draw_index = 0u;
        if (index_frame != g_stats.frames) {
            index_frame = g_stats.frames;
            draw_index = 0u;
        }
        const std::uint32_t index = draw_index++;
        if (trace_frame >= 0 && g_stats.frames == static_cast<std::uint64_t>(trace_frame)) {
            const std::uint32_t texture = (cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u);
            const bool samples_target = textured && (texture & 0x0F000000u) == 0x04000000u && g_targets.count(target_key(texture)) != 0u;
            std::fprintf(stderr,
                         "[tt] %u target %06X n=%u through=%d masked=%d blend=%d(%06X) sample=%s%06X tex=%08X clear=%d ztest=%d zf=%u zw=%d "
                         "zscale=%g zoff=%g fog=%d tfunc=%05X alpha=%d/%06X cull=%d\n",
                         index, target_key(address), g_batch_count, through ? 1 : 0, g_state.color_mask == 0 ? 1 : 0,
                         g_state.blend == 1 ? 1 : 0, cmd(0xDFu) & 0xFFFFFFu, samples_target ? "T" : "-",
                         samples_target ? target_key(texture) : 0u, textured ? texture : 0u, clear_mode ? 1 : 0,
                         (cmd(0x23u) & 1u) != 0u ? 1 : 0, cmd(0xDEu) & 7u, (cmd(0xE7u) & 1u) == 0u ? 1 : 0, float24(cmd(0x44u)),
                         float24(cmd(0x47u)), fog ? 1 : 0, texfunc & 0x1FFFFu, alpha_enabled ? 1 : 0, alpha_test & 0xFFFFFFu,
                         (cmd(0x1Du) & 1u) != 0u ? 1 : 0);
            std::fprintf(stderr, "[tt]    stencil test %u func %06X op %06X; alpha blend fix %06X/%06X; mask %06X/%06X; logic %u\n",
                         cmd(0x24u) & 1u, cmd(0xDCu) & 0xFFFFFFu, cmd(0xDDu) & 0xFFFFFFu, cmd(0xE0u) & 0xFFFFFFu,
                         cmd(0xE1u) & 0xFFFFFFu, cmd(0xE8u) & 0xFFFFFFu, cmd(0xE9u) & 0xFFFFFFu, cmd(0x28u) & 1u);
            if (textured)
                std::fprintf(stderr, "[tt]    texture bufw=%u size=%ux%u format=%u fb format=%u\n", cmd(0xA8u) & 0x7FFu,
                             1u << (cmd(0xB8u) & 0xFu), 1u << ((cmd(0xB8u) >> 8u) & 0xFu), cmd(0xC3u) & 0xFu, cmd(0xD2u) & 3u);
            static const std::pair<long, long> skip = [] {
                const char *v = std::getenv("PSPRECOMP_BURNOUT_TARGET_SKIP");
                long a = -1, b = -1;
                if (v != nullptr && std::sscanf(v, "%ld-%ld", &a, &b) != 2) b = a;
                return std::pair<long, long>{a, b};
            }();
            if (static_cast<long>(index) >= skip.first && static_cast<long>(index) <= skip.second) {
                g_batch.clear();
                g_batch_count = 0u;
                g_batch_in_place = nullptr;
                g_batch_indices.clear();
                g_batch_runs.clear();
                g_batch_index_total = 0u;
                phase = outer_phase;
                return;
            }
        }
    }
    const auto skipped_cutout = [&] {
        if (!g_draw_is_cutout || g_skipped_cutouts.empty()) return false;
        const std::uint32_t texture = (cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u);
        return std::find(g_skipped_cutouts.begin(), g_skipped_cutouts.end(), texture) != g_skipped_cutouts.end();
    };
    if ((g_no_cutouts && g_draw_is_cutout && g_texture_foliage) || skipped_cutout()) {
        g_batch.clear();
        g_batch_count = 0u;
        g_batch_in_place = nullptr;
        g_batch_indices.clear();
        g_batch_runs.clear();
        g_batch_index_total = 0u;
        phase = outer_phase;
        return;
    }
    phase = 9u;
    bind_vertices();
    ++g_stats.batches;
    g_stats.vertices += g_batch_count;
    g_batch.clear();
    g_batch_count = 0u;
    g_batch_in_place = nullptr;
    g_batch_indices.clear();
    g_batch_runs.clear();
    g_batch_index_total = 0u;
    if (timed) g_stats.draw_ms += (host_now_ms() - started) * 16.0;
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
    bool same_offsets{};  // only the stride differs (padding after the position)
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
    layout.same_offsets = (tc == 0u || psp.tc == layout.tc) && (layout.col_size == 0u || psp.col == layout.col) &&
                          (nrm == 0u || psp.nrm == layout.nrm) && psp.pos == layout.pos && psp.stride <= layout.stride &&
                          (vtype & 0x600u) == 0u;  // no weights in front
    layout.same_as_psp = layout.same_offsets && psp.stride == layout.stride;
    return layout;
}

void copy_vertex(std::uint8_t *out, const std::uint8_t *in, const BatchLayout &to, const VertexLayout &from) {
    std::memcpy(out + to.tc, in + from.tc, to.tc_size * 2u);
    std::memcpy(out + to.col, in + from.col, to.col_size);
    std::memcpy(out + to.nrm, in + from.nrm, to.nrm_size * 3u);
    std::memcpy(out + to.pos, in + from.pos, to.pos_size * 3u);
}

BatchLayout g_layout;

// Vertices whose fields already sit at the batch offsets: one fixed-size copy
// each (the common odd strides get a constant size the compiler inlines).
template <std::uint32_t N>
void copy_strided(std::uint8_t *out, std::uint32_t out_stride, const std::uint8_t *in, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i, out += out_stride, in += N) std::memcpy(out, in, N);
}

void copy_vertices(std::uint8_t *out, const std::uint8_t *in, std::uint32_t count, const BatchLayout &to,
                   const VertexLayout &from) {
    if (!to.same_offsets) {
        for (std::uint32_t i = 0; i < count; ++i) copy_vertex(out + i * to.stride, in + i * from.stride, to, from);
        return;
    }
    switch (from.stride) {
    case 6u: copy_strided<6u>(out, to.stride, in, count); return;
    case 10u: copy_strided<10u>(out, to.stride, in, count); return;
    case 14u: copy_strided<14u>(out, to.stride, in, count); return;
    case 18u: copy_strided<18u>(out, to.stride, in, count); return;
    case 22u: copy_strided<22u>(out, to.stride, in, count); return;
    default:
        for (std::uint32_t i = 0; i < count; ++i) std::memcpy(out + i * to.stride, in + i * from.stride, from.stride);
    }
}

// The sun flare's measuring box: the game draws a black quad at the top of the
// screen (blending off, stencil test on), the sun sprite into it, and reads
// the result back from VRAM, which the GL renderer never writes. Left as is it
// shows as a black square around a second sun; the quad, every through draw
// covering exactly the same rectangle and every draw sampling the small target
// the box is copied into are left out (the sky sun stays).
struct ScreenBox {
    float x0{}, y0{}, x1{}, y1{};
    bool valid{};
};
ScreenBox g_sun_box;
std::uint32_t g_sun_copy{UINT32_MAX};  // target key of the box's copy

// Bounding box of a through-mode batch's float texture coordinates.
bool batch_uv_box(ScreenBox &box) {
    if ((g_batch_vtype & 0x800000u) == 0u || (g_batch_vtype & 3u) != 3u || g_batch_count == 0u) return false;
    const std::uint8_t *vertices = g_batch_in_place != nullptr ? g_batch_in_place : g_batch.data();
    box = {1e9f, 1e9f, -1e9f, -1e9f, true};
    for (std::uint32_t i = 0; i < g_batch_count; ++i) {
        float uv[2];
        std::memcpy(uv, vertices + i * g_layout.stride + g_layout.tc, sizeof(uv));
        box.x0 = std::min(box.x0, uv[0]);
        box.y0 = std::min(box.y0, uv[1]);
        box.x1 = std::max(box.x1, uv[0]);
        box.y1 = std::max(box.y1, uv[1]);
    }
    return true;
}

// Bounding box of a through-mode batch with float positions.
bool batch_box(ScreenBox &box) {
    if ((g_batch_vtype & 0x800000u) == 0u || ((g_batch_vtype >> 7u) & 3u) != 3u || g_batch_count == 0u) return false;
    const std::uint8_t *vertices = g_batch_in_place != nullptr ? g_batch_in_place : g_batch.data();
    box = {1e9f, 1e9f, -1e9f, -1e9f, true};
    for (std::uint32_t i = 0; i < g_batch_count; ++i) {
        float xy[2];
        std::memcpy(xy, vertices + i * g_layout.stride + g_layout.pos, sizeof(xy));
        box.x0 = std::min(box.x0, xy[0]);
        box.y0 = std::min(box.y0, xy[1]);
        box.x1 = std::max(box.x1, xy[0]);
        box.y1 = std::max(box.y1, xy[1]);
    }
    return true;
}

bool is_sun_box_draw(bool textured) {
    if (textured && g_sun_box.valid && g_sun_copy != UINT32_MAX &&
        target_key((cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u)) == g_sun_copy)
        return true;
    ScreenBox box;
    if (g_batch_count > 8u || !batch_box(box)) return false;
    if (!textured && (cmd(0x21u) & 1u) == 0u && (cmd(0x24u) & 1u) != 0u && box.x1 - box.x0 <= 64.0f &&
        box.y1 - box.y0 <= 64.0f && g_layout.col_size == 4u) {
        const std::uint8_t *vertices = g_batch_in_place != nullptr ? g_batch_in_place : g_batch.data();
        for (std::uint32_t i = 0; i < g_batch_count; ++i) {
            std::uint32_t color;
            std::memcpy(&color, vertices + i * g_layout.stride + g_layout.col, sizeof(color));
            if (color != 0u) return false;
        }
        g_sun_box = box;
        return true;
    }
    if (!g_sun_box.valid) return false;
    if (textured) {
        const std::uint32_t texture = (cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u);
        if (target_key(texture) == g_sun_copy) return true;
        ScreenBox uv;
        const auto close_to = [](float a, float b) { return std::abs(a - b) <= 2.0f; };
        if (target_key(cmd(0x9Cu)) != target_key(texture) && g_targets.count(target_key(texture)) != 0u && batch_uv_box(uv) &&
            close_to(uv.x0, g_sun_box.x0) && close_to(uv.y0, g_sun_box.y0) && close_to(uv.x1, g_sun_box.x1) && close_to(uv.y1, g_sun_box.y1)) {
            g_sun_copy = target_key(cmd(0x9Cu));  // the copy itself is harmless
            return false;
        }
    }
    return g_sun_box.valid && box.x0 == g_sun_box.x0 && box.y0 == g_sun_box.y0 && box.x1 == g_sun_box.x1 &&
           box.y1 == g_sun_box.y1;
}


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
std::uint32_t upload_vertex_state(bool through) {
    const std::uint32_t vtype = g_batch_vtype;
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const float tc_scale = through ? 1.0f : (tc == 1u ? 1.0f / 128.0f : tc == 2u ? 1.0f / 32768.0f : 1.0f);
    const float pos_scale = through ? 1.0f : (pos == 1u ? 1.0f / 128.0f : pos == 2u ? 1.0f / 32768.0f : 1.0f);
    const float nrm_scale = nrm == 1u ? 1.0f / 127.0f : nrm == 2u ? 1.0f / 32767.0f : 1.0f;
    uniform4(kFormat, pos_scale, tc_scale, nrm_scale, static_cast<float>(col >= 4u ? col : 0u));
    const bool lit = !through && (cmd(0x17u) & 1u) != 0u;
    const std::uint32_t map_mode = cmd(0xC0u) & 3u, map_source = (cmd(0xC0u) >> 8u) & 3u;
    const float mode = through ? 3.0f : (map_mode == 1u || map_mode == 2u) ? static_cast<float>(map_mode) : 0.0f;
    uniform4(kTexgen, mode, static_cast<float>(map_source), through && pos == 2u ? 1.0f : 0.0f, lit ? 1.0f : 0.0f);
    if (through) {
        const float width = static_cast<float>(1u << std::min<std::uint32_t>(cmd(0xB8u) & 0xFu, 10u));
        const float height = static_cast<float>(1u << std::min<std::uint32_t>((cmd(0xB8u) >> 8u) & 0xFu, 10u));
        uniform4(kUvScale, 1.0f / width, 1.0f / height, 0.0f, 0.0f);
    } else if (mode == 0.0f) {
        uniform4(kUvScale, float24(cmd(0x48u)), float24(cmd(0x49u)), float24(cmd(0x4Au)), float24(cmd(0x4Bu)));
    } else if (mode == 1.0f) {
        float texmatrix[16];
        psp_matrix(g_texgen.data(), texmatrix);
        uniform_matrix(kTexMatrix, texmatrix);
    } else {
        const std::uint32_t ls = cmd(0xC1u);
        const auto direction = [&](std::uint32_t l) {
            return normalized({float24(cmd(0x63u + l * 3u)), float24(cmd(0x64u + l * 3u)), float24(cmd(0x65u + l * 3u))});
        };
        const auto lu = direction(ls & 3u), lv = direction((ls >> 8u) & 3u);
        const float shade[8]{lu[0], lu[1], lu[2], 0.0f, lv[0], lv[1], lv[2], 0.0f};
        uniform4v(kShade, shade, 2);
    }
    static bool lighting_has_color = false;
    if (lit && (g_lighting_dirty || lighting_has_color != (col >= 4u))) {
        g_lighting_dirty = false;
        lighting_has_color = col >= 4u;
        const auto lighting = lighting_uniforms(lighting_has_color);
        uniform4v(kLighting, lighting.data(), 30);
    }
    // Vertex half of the program variant (bit layout in variant_defines).
    const std::uint32_t mode_bits = static_cast<std::uint32_t>(mode);
    return (through ? 1u : 0u) | (lit ? 2u : 0u) | mode_bits << 2u | (mode_bits == 1u ? map_source << 4u : 0u) |
           (col >= 4u && col <= 6u ? col : 0u) << 6u;
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
    static const bool merge_stats = std::getenv("PSPRECOMP_BURNOUT_MERGE_STATS") != nullptr;
    if (merge_stats) {
        // Diagnostic: how many cut-out draws directly follow a cut-out draw
        // with the same program, texture and vertex type (mergeable but for
        // the uniforms, i.e. the world matrix), and how many ops lie between.
        static bool last_cutout = false;
        static std::uint32_t last_program = 0, last_texture = 0, last_vtype = 0;
        static std::size_t last_end = 0;
        static double cutouts = 0, same = 0, same_tex = 0, between = 0, frames_at = 0, vertices = 0;
        static double hist[5]{};
        if (g_draw_is_cutout) {
            const float c = g_texture_clear;
            ++hist[c == 0.0f ? 0 : c < 0.01f ? 1 : c < 0.05f ? 2 : c < 0.2f ? 3 : 4];
            ++cutouts;
            vertices += g_batch_count;
            if (last_cutout && last_texture == g_state.texture) {
                ++same_tex;
                if (last_program == g_state.program && last_vtype == g_batch_vtype) {
                    ++same;
                    between += static_cast<double>(g_frame.ops.size() - last_end);
                }
            }
        }
        last_cutout = g_draw_is_cutout;
        last_program = g_state.program;
        last_texture = g_state.texture;
        last_vtype = g_batch_vtype;
        last_end = g_frame.ops.size();
        static std::uint64_t last_frame = 0;
        if (g_stats.frames != last_frame && g_stats.frames % 300u == 0u) {
            last_frame = g_stats.frames;
            std::fprintf(stderr, "[merge] per frame: %.0f cut-out draws (%.0f vertices), %.0f follow one with the same texture, %.0f also same program and vtype (%.1f op words between)\n",
                         cutouts / 300.0, vertices / 300.0, same_tex / 300.0, same / 300.0, same > 0 ? between / same : 0.0);
            std::fprintf(stderr, "[merge] cut-out textures by clear texels: none %.0f, <1%% %.0f, <5%% %.0f, <20%% %.0f, more %.0f\n",
                         hist[0] / 300.0, hist[1] / 300.0, hist[2] / 300.0, hist[3] / 300.0, hist[4] / 300.0);
            for (double &h : hist) h = 0;
            cutouts = same = same_tex = between = vertices = 0;
            (void)frames_at;
        }
    }
    if (g_prepass_eligible) {
        record(Op::Prepass, {1u});
        ++g_prepassed;
    }
    record(Op::Draw, {in_frame, source, static_cast<std::uint32_t>(bytes), g_layout.stride,
                      record_data(attribs.data(), sizeof(attribs)),
                      record_data(g_batch_runs.data(), g_batch_runs.size() * sizeof(std::uint32_t)),
                      static_cast<std::uint32_t>(g_batch_runs.size()),
                      record_data(g_batch_indices.data(), g_batch_indices.size() * sizeof(std::uint16_t)),
                      g_batch_index_total});
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

// PSP skinning: the weighted sum of the vertex transformed by each bone
// matrix (4x3, row vector), done on the CPU. The result is a vertex type with
// float position and normal and no weights; the world matrix follows as usual.
std::array<float, 8 * 12> g_bones{};
// Cut-out merging (test): unlit alpha-tested textured PRIMs get the world
// matrix applied as they are decoded (float positions, world = identity in the
// shader), so world matrix changes no longer end their batch and the trees,
// fences and bushes go down in a few draws instead of ~150. On the Vita's GPU
// every alpha-tested draw is a punch-through pass over the tiles it covers.
bool g_merge_cutouts{std::getenv("PSPRECOMP_BURNOUT_MERGE_CUTOUTS") != nullptr};
bool g_merge_all{std::getenv("PSPRECOMP_BURNOUT_MERGE_ALL") != nullptr};
std::vector<std::uint8_t> g_world_vertices;
std::uint64_t g_world_prims{};
std::uint32_t g_bone_cursor{};
ScratchVector<std::uint8_t> g_skinned;

std::uint32_t skinned_vtype(std::uint32_t vtype) {
    const std::uint32_t nrm = (vtype >> 5u) & 3u;
    return (vtype & 0x1Fu) | (nrm != 0u ? 3u << 5u : 0u) | 3u << 7u | (vtype & 0x1800u);
}

float component(const std::uint8_t *at, std::uint32_t format, std::uint32_t k, bool is_signed) {
    switch (format) {
    case 1u: return is_signed ? static_cast<float>(static_cast<std::int8_t>(at[k])) : static_cast<float>(at[k]);
    case 2u: {
        std::uint16_t v;
        std::memcpy(&v, at + k * 2u, 2u);
        return is_signed ? static_cast<float>(static_cast<std::int16_t>(v)) : static_cast<float>(v);
    }
    default: {
        float v;
        std::memcpy(&v, at + k * 4u, 4u);
        return v;
    }
    }
}

const std::uint8_t *skin(const std::uint8_t *raw, std::uint32_t count, std::uint32_t vtype, const VertexLayout &from,
                         const VertexLayout &to) {
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const std::uint32_t weight = (vtype >> 9u) & 3u, weights = ((vtype >> 14u) & 7u) + 1u;
    const float weight_scale = weight == 1u ? 1.0f / 128.0f : weight == 2u ? 1.0f / 32768.0f : 1.0f;
    const float pos_scale = pos == 1u ? 1.0f / 128.0f : pos == 2u ? 1.0f / 32768.0f : 1.0f;
    const float nrm_scale = nrm == 1u ? 1.0f / 127.0f : nrm == 2u ? 1.0f / 32767.0f : 1.0f;
    const std::uint32_t pos_format = std::max<std::uint32_t>(pos, 1u);
    g_skinned.resize(static_cast<std::size_t>(count) * to.stride);
#if defined(__ARM_NEON)
    // The Vita's VFP runs the scalar sums below as long chains of dependent
    // multiply-adds (~1.7 us a vertex, 10 ms a race frame). NEON: blend the
    // bone matrices by weight first (four rows of four lanes), then transform
    // the position and normal once.
    float32x4_t rows[8][4];
    for (std::uint32_t b = 0; b < weights && b < 8u; ++b) {
        const float *m = g_bones.data() + b * 12u;
        for (std::uint32_t r = 0; r < 4u; ++r) {
            const float row[4]{m[r * 3u], m[r * 3u + 1u], m[r * 3u + 2u], 0.0f};
            rows[b][r] = vld1q_f32(row);
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *in = raw + static_cast<std::size_t>(i) * from.stride;
        std::uint8_t *out = g_skinned.data() + static_cast<std::size_t>(i) * to.stride;
        std::memcpy(out + to.tc, in + from.tc, sizes_tc[tc] * 2u);
        std::memcpy(out + to.col, in + from.col, sizes_col[col]);
        float32x4_t b0 = vdupq_n_f32(0.0f), b1 = b0, b2 = b0, b3 = b0;
        for (std::uint32_t b = 0; b < weights && b < 8u; ++b) {
            const float w = component(in, weight, b, false) * weight_scale;
            if (w == 0.0f) continue;
            b0 = vmlaq_n_f32(b0, rows[b][0], w);
            b1 = vmlaq_n_f32(b1, rows[b][1], w);
            b2 = vmlaq_n_f32(b2, rows[b][2], w);
            b3 = vmlaq_n_f32(b3, rows[b][3], w);
        }
        const float px = component(in + from.pos, pos_format, 0u, true) * pos_scale;
        const float py = component(in + from.pos, pos_format, 1u, true) * pos_scale;
        const float pz = component(in + from.pos, pos_format, 2u, true) * pos_scale;
        float32x4_t sp = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(b3, b0, px), b1, py), b2, pz);
        float result[4];
        vst1q_f32(result, sp);
        std::memcpy(out + to.pos, result, 12u);
        if (nrm != 0u) {
            const float nx = component(in + from.nrm, nrm, 0u, true) * nrm_scale;
            const float ny = component(in + from.nrm, nrm, 1u, true) * nrm_scale;
            const float nz = component(in + from.nrm, nrm, 2u, true) * nrm_scale;
            const float32x4_t sn = vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(b0, nx), b1, ny), b2, nz);
            vst1q_f32(result, sn);
            std::memcpy(out + to.nrm, result, 12u);
        }
    }
    return g_skinned.data();
#endif
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *in = raw + static_cast<std::size_t>(i) * from.stride;
        std::uint8_t *out = g_skinned.data() + static_cast<std::size_t>(i) * to.stride;
        std::memcpy(out + to.tc, in + from.tc, sizes_tc[tc] * 2u);
        std::memcpy(out + to.col, in + from.col, sizes_col[col]);
        float p[3], n[3], sp[3]{}, sn[3]{};
        for (std::uint32_t k = 0; k < 3u; ++k) {
            p[k] = component(in + from.pos, pos_format, k, true) * pos_scale;
            n[k] = nrm != 0u ? component(in + from.nrm, nrm, k, true) * nrm_scale : 0.0f;
        }
        for (std::uint32_t b = 0; b < weights; ++b) {
            const float w = component(in, weight, b, false) * weight_scale;
            if (w == 0.0f) continue;
            const float *m = g_bones.data() + b * 12u;
            for (std::uint32_t c = 0; c < 3u; ++c) {
                sp[c] += w * (p[0] * m[c] + p[1] * m[3u + c] + p[2] * m[6u + c] + m[9u + c]);
                sn[c] += w * (n[0] * m[c] + n[1] * m[3u + c] + n[2] * m[6u + c]);
            }
        }
        std::memcpy(out + to.pos, sp, sizeof(sp));
        if (nrm != 0u) std::memcpy(out + to.nrm, sn, sizeof(sn));
    }
    return g_skinned.data();
}

const std::uint8_t *world_transform(const std::uint8_t *raw, std::uint32_t count, std::uint32_t vtype,
                                    const VertexLayout &from, const VertexLayout &to) {
    const std::uint32_t tc = vtype & 3u, col = (vtype >> 2u) & 7u, nrm = (vtype >> 5u) & 3u, pos = (vtype >> 7u) & 3u;
    const float pos_scale = pos == 1u ? 1.0f / 128.0f : pos == 2u ? 1.0f / 32768.0f : 1.0f;
    const float nrm_scale = nrm == 1u ? 1.0f / 127.0f : nrm == 2u ? 1.0f / 32767.0f : 1.0f;
    const std::uint32_t pos_format = std::max<std::uint32_t>(pos, 1u);
    const float *m = g_world.data();
    g_world_vertices.resize(static_cast<std::size_t>(count) * to.stride);
#if defined(__ARM_NEON)
    // NEON: the rows as vectors, one multiply-add per row (the scalar chain
    // below runs as dependent VFP multiply-adds on the Vita).
    const float row0[4]{m[0], m[1], m[2], 0.0f}, row1[4]{m[3], m[4], m[5], 0.0f}, row2[4]{m[6], m[7], m[8], 0.0f},
        row3[4]{m[9], m[10], m[11], 0.0f};
    const float32x4_t r0 = vld1q_f32(row0), r1 = vld1q_f32(row1), r2 = vld1q_f32(row2), r3 = vld1q_f32(row3);
    const std::size_t tc_bytes = sizes_tc[tc] * 2u, col_bytes = sizes_col[col];
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *in = raw + static_cast<std::size_t>(i) * from.stride;
        std::uint8_t *out = g_world_vertices.data() + static_cast<std::size_t>(i) * to.stride;
        std::memcpy(out + to.tc, in + from.tc, tc_bytes);
        std::memcpy(out + to.col, in + from.col, col_bytes);
        float px, py, pz;
        if (pos_format == 2u) {
            std::int16_t v[3];
            std::memcpy(v, in + from.pos, sizeof(v));
            px = static_cast<float>(v[0]) * pos_scale;
            py = static_cast<float>(v[1]) * pos_scale;
            pz = static_cast<float>(v[2]) * pos_scale;
        } else {
            px = component(in + from.pos, pos_format, 0u, true) * pos_scale;
            py = component(in + from.pos, pos_format, 1u, true) * pos_scale;
            pz = component(in + from.pos, pos_format, 2u, true) * pos_scale;
        }
        float result[4];
        vst1q_f32(result, vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(r3, r0, px), r1, py), r2, pz));
        std::memcpy(out + to.pos, result, 12u);
        if (nrm != 0u) {
            const float nx = component(in + from.nrm, nrm, 0u, true) * nrm_scale;
            const float ny = component(in + from.nrm, nrm, 1u, true) * nrm_scale;
            const float nz = component(in + from.nrm, nrm, 2u, true) * nrm_scale;
            vst1q_f32(result, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(r0, nx), r1, ny), r2, nz));
            std::memcpy(out + to.nrm, result, 12u);
        }
    }
    return g_world_vertices.data();
#endif
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *in = raw + static_cast<std::size_t>(i) * from.stride;
        std::uint8_t *out = g_world_vertices.data() + static_cast<std::size_t>(i) * to.stride;
        std::memcpy(out + to.tc, in + from.tc, sizes_tc[tc] * 2u);
        std::memcpy(out + to.col, in + from.col, sizes_col[col]);
        const float px = component(in + from.pos, pos_format, 0u, true) * pos_scale;
        const float py = component(in + from.pos, pos_format, 1u, true) * pos_scale;
        const float pz = component(in + from.pos, pos_format, 2u, true) * pos_scale;
        float sp[3];
        for (std::uint32_t c = 0; c < 3u; ++c) sp[c] = px * m[c] + py * m[3u + c] + pz * m[6u + c] + m[9u + c];
        std::memcpy(out + to.pos, sp, sizeof(sp));
        if (nrm != 0u) {
            const float nx = component(in + from.nrm, nrm, 0u, true) * nrm_scale;
            const float ny = component(in + from.nrm, nrm, 1u, true) * nrm_scale;
            const float nz = component(in + from.nrm, nrm, 2u, true) * nrm_scale;
            float sn[3];
            for (std::uint32_t c = 0; c < 3u; ++c) sn[c] = nx * m[c] + ny * m[3u + c] + nz * m[6u + c];
            std::memcpy(out + to.nrm, sn, sizeof(sn));
        }
    }
    return g_world_vertices.data();
}

// Skinned results by guest vertex address: the cars' bones (body damage)
// rarely change, so a PRIM whose source vertices and bones are unchanged since
// it was last skinned reuses the result. On the Vita the skinning itself ran
// ~1.5 us a vertex (~10 ms a race frame on the worker).
struct SkinEntry {
    std::uint32_t vtype{}, count{};
    std::uint64_t used_frame{};
    std::vector<std::uint8_t> source;
    std::array<float, 8 * 12> bones{};
    std::vector<std::uint8_t> result;
};
std::unordered_map<std::uint64_t, SkinEntry> g_skin_cache;

// memcmp(a, b, n) == 0 for the skin cache: both sides are usually cold (the
// snapshot was just copied on the other core, the entry was last read a
// frame ago), so lines ahead are fetched while these compare.
inline void skin_prefetch(const void *address) noexcept {
#if defined(_MSC_VER)
    _mm_prefetch(static_cast<const char *>(address), _MM_HINT_T0);
#else
    __builtin_prefetch(address);
#endif
}

bool same_bytes(const std::uint8_t *a, const std::uint8_t *b, std::size_t n) noexcept {
    std::size_t i = 0u;
    for (std::size_t at = 0u; at < n && at < 128u; at += 32u) {
        skin_prefetch(a + at);
        skin_prefetch(b + at);
    }
    for (; i + 32u <= n; i += 32u) {
        skin_prefetch(a + i + 128u);
        skin_prefetch(b + i + 128u);
        std::uint32_t diff = 0u;
        for (std::size_t j = 0u; j < 32u; j += 4u) {
            std::uint32_t x, y;
            std::memcpy(&x, a + i + j, 4u);
            std::memcpy(&y, b + i + j, 4u);
            diff |= x ^ y;
        }
        if (diff != 0u) return false;
    }
    return std::memcmp(a + i, b + i, n - i) == 0;
}

const std::uint8_t *skin_cached(const std::uint8_t *raw, std::uint32_t count, std::uint32_t vtype, const VertexLayout &from,
                                const VertexLayout &to) {
    const std::size_t source_bytes = static_cast<std::size_t>(count) * from.stride;
    const std::uint32_t bone_floats = (((vtype >> 14u) & 7u) + 1u) * 12u;
    const std::uint64_t key = (static_cast<std::uint64_t>(g_vertex_address) << 32u) ^ (static_cast<std::uint64_t>(count) << 8u) ^
                              (static_cast<std::uint64_t>(g_bone_cursor) << 1u);
    SkinEntry &entry = g_skin_cache[key];
    const bool hit = entry.vtype == vtype && entry.count == count && entry.source.size() == source_bytes &&
                     std::memcmp(entry.bones.data(), g_bones.data(), bone_floats * sizeof(float)) == 0 &&
                     same_bytes(entry.source.data(), raw, source_bytes);
    entry.used_frame = g_stats.frames;
    if (hit) {
        ++g_gpu.skin_hits;
        return entry.result.data();
    }
    const std::uint8_t *skinned = skin(raw, count, vtype, from, to);
    entry.vtype = vtype;
    entry.count = count;
    entry.source.assign(raw, raw + source_bytes);
    std::memcpy(entry.bones.data(), g_bones.data(), sizeof(entry.bones));
    entry.result.assign(skinned, skinned + static_cast<std::size_t>(count) * to.stride);
    // Forget what has not been drawn for a while.
    if (g_skin_cache.size() > 512u)
        for (auto it = g_skin_cache.begin(); it != g_skin_cache.end();)
            it = g_stats.frames - it->second.used_frame > 120u ? g_skin_cache.erase(it) : std::next(it);
    return entry.result.data();
}

bool decode_prim(psprecomp::GuestMemory &memory, std::uint32_t data) {
    phase = 1u;
    const std::uint32_t primitive = (data >> 16u) & 7u;
    const std::uint32_t count = data & 0xFFFFu;
    const std::uint32_t psp_vtype = cmd(0x12u);
    const std::uint32_t weight = (psp_vtype >> 9u) & 3u, index = (psp_vtype >> 11u) & 3u;
    const std::uint32_t weights = weight != 0u ? ((psp_vtype >> 14u) & 7u) + 1u : 0u;
    const std::uint32_t morphs = ((psp_vtype >> 18u) & 7u) + 1u;
    const bool through = (psp_vtype & 0x800000u) != 0u;

    // The vertex type rarely changes between PRIMs.
    static std::uint32_t layout_vtype = UINT32_MAX;
    static VertexLayout psp_layout;
    if (psp_vtype != layout_vtype) {
        layout_vtype = psp_vtype;
        psp_layout = vertex_layout(psp_vtype);
    }
    const std::uint32_t stride = psp_layout.stride;

    // A skipped frame (frame skip) only follows the vertex and index pointers.
    if (!g_drawing) {
        if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
        else g_vertex_address += count * stride;
        phase = 0u;
        return false;
    }
    if (morphs > 1u || primitive < 3u || (weights != 0u && through)) {  // morphing/points/lines: not yet
        ++g_stats.skipped_primitives;
        static std::unordered_map<std::uint64_t, int> seen;
        if (seen[(std::uint64_t{psp_vtype} << 8u) | primitive]++ == 0) {
            char line[96];
            std::snprintf(line, sizeof(line), "skipped PRIM %u, vtype %06X, %u vertices", primitive, psp_vtype, count);
            trace(line);
        }
        if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
        else g_vertex_address += count * stride;
        return false;
    }

    const bool lit = !through && (cmd(0x17u) & 1u) != 0u;

    phase = 2u;
    std::uint32_t max_vertex = 0u;
    static ScratchVector<std::uint32_t> indices;
    indices.resize(count);
    // Flat shading needs per-triangle colours, so it copies vertices.
    const bool flat = (cmd(0x50u) & 1u) == 0u && (cmd(0xD3u) & 1u) == 0u;
    if (index == 0u) {
        // Consecutive lists, strips and fans never look at the indices.
        if (flat || primitive == 6u)
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
    static const bool foliage_stats = std::getenv("PSPRECOMP_BURNOUT_FOLIAGE_STATS") != nullptr || g_thin_foliage;
    if ((g_thin_foliage || foliage_stats) && weights == 0u && !through && (cmd(0x1Eu) & 1u) != 0u && (cmd(0x22u) & 1u) != 0u &&
        (cmd(0xDBu) & 7u) != 1u && (cmd(0x21u) & 1u) == 0u && vertex_count != 0u &&
        g_foliage_addresses.count((cmd(0xA0u) & 0xFFFFF0u) | ((cmd(0xA8u) << 8u) & 0x0F000000u)) != 0u) {
        // Distance from the camera of the PRIM's first and last vertices (nearest).
        const std::uint32_t pos = (psp_vtype >> 7u) & 3u;
        const float pos_scale = pos == 1u ? 1.0f / 128.0f : pos == 2u ? 1.0f / 32768.0f : 1.0f;
        const std::uint32_t pos_format = std::max<std::uint32_t>(pos, 1u);
        float nearest = 1e30f;
        for (const std::uint32_t v : {0u, vertex_count - 1u}) {
            const std::uint8_t *at = raw + static_cast<std::size_t>(v) * stride + psp_layout.pos;
            const float p[3]{component(at, pos_format, 0u, true) * pos_scale, component(at, pos_format, 1u, true) * pos_scale,
                             component(at, pos_format, 2u, true) * pos_scale};
            float w[3], e[3];
            for (int c = 0; c < 3; ++c) w[c] = p[0] * g_world[c] + p[1] * g_world[3 + c] + p[2] * g_world[6 + c] + g_world[9 + c];
            for (int c = 0; c < 3; ++c) e[c] = w[0] * g_view[c] + w[1] * g_view[3 + c] + w[2] * g_view[6 + c] + g_view[9 + c];
            nearest = std::min(nearest, std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
        }
        if (foliage_stats) {
            static double bins[10]{}, vbins[10]{};
            static std::uint64_t last_frame = 0;
            int bin = 0;
            for (float edge = 16.0f; bin < 9 && nearest >= edge; edge *= 2.0f) ++bin;
            bins[bin] += 1.0;
            vbins[bin] += vertex_count;
            if (g_stats.frames != last_frame && g_stats.frames % 300u == 0u) {
                last_frame = g_stats.frames;
                std::fprintf(stderr, "[foliage] PRIMs (vertices) per frame by distance <16 <32 <64 <128 <256 <512 <1k <2k <4k more:");
                for (int i = 0; i < 10; ++i) std::fprintf(stderr, " %.0f(%.0f)", bins[i] / 300.0, vbins[i] / 300.0);
                std::fprintf(stderr, "; thinned %.0f PRIMs (%.0f vertices)\n", g_thinned_prims / 300.0, g_thinned_vertices / 300.0);
                for (int i = 0; i < 10; ++i) bins[i] = vbins[i] = 0.0;
                g_thinned_prims = g_thinned_vertices = 0u;
            }
        }
        if (g_thin_foliage && nearest > g_thin_near) {
            // Kept: 2 in 3 between near and far, 1 in 3 beyond.
            const std::uint32_t h = (g_vertex_address * 2654435761u) >> 28u;  // 0..15, the same for the same object
            const bool keep = nearest > g_thin_far ? h < 6u : h < 11u;
            if (!keep) {
                ++g_thinned_prims;
                g_thinned_vertices += vertex_count;
                if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
                else g_vertex_address += count * stride;
                phase = 0u;
                return false;
            }
        }
    }
    // Skinned vertices are blended here and drawn as plain float ones.
    std::uint32_t vtype = psp_vtype;
    const VertexLayout *source_layout = &psp_layout;
    static std::uint32_t skinned_from = UINT32_MAX;
    static VertexLayout skinned_layout;
    if (weights != 0u) {
        vtype = skinned_vtype(psp_vtype);
        if (skinned_from != psp_vtype) {
            skinned_from = psp_vtype;
            skinned_layout = vertex_layout(vtype);
        }
        g_gpu.skinned += vertex_count;
        g_gpu.weights += static_cast<std::uint64_t>(vertex_count) * (((psp_vtype >> 14u) & 7u) + 1u);
        const double skin_started = host_now_ms();
        raw = skin_cached(raw, vertex_count, psp_vtype, psp_layout, skinned_layout);
        g_gpu.skin_ms += host_now_ms() - skin_started;
        ++g_gpu.skin_calls;
        source_layout = &skinned_layout;
    }
    const bool merging = g_merge_cutouts || (g_gpu_probe && g_probe_mode.load(std::memory_order_relaxed) == 5u);
    // (merge_all: every unlit unblended PRIM, alpha-tested or not: fewer draws.)
    const bool world_applied = merging && weights == 0u && !through && !lit &&
                               (g_merge_all || ((cmd(0x1Eu) & 1u) != 0u && (cmd(0x22u) & 1u) != 0u && (cmd(0xDBu) & 7u) != 1u)) &&
                               (cmd(0x21u) & 1u) == 0u && (cmd(0xD3u) & 1u) == 0u && (cmd(0xC0u) & 3u) == 0u;
    static std::uint32_t world_from = UINT32_MAX;
    static VertexLayout world_layout;
    if (world_applied) {
        vtype = skinned_vtype(psp_vtype);
        if (world_from != psp_vtype) {
            world_from = psp_vtype;
            world_layout = vertex_layout(vtype);
        }
        raw = world_transform(raw, vertex_count, psp_vtype, psp_layout, world_layout);
        source_layout = &world_layout;
        ++g_world_prims;
    }
    const VertexLayout &layout = *source_layout;
    const std::uint32_t vertex_stride = layout.stride;

    if (!g_batch_runs.empty() && (g_batch_vtype != vtype || g_batch_world != world_applied)) flush();
    if (g_batch_count == 0u) {
        g_batch_vtype = vtype;
        g_layout = batch_layout(vtype);
        g_batch_min_alpha = 255u;
        g_batch_world = world_applied;
    }
    if (lit) g_stats.lit_vertices += vertex_count;
    // Indexed batch: the PRIM's vertices are appended once and triangles index
    // them.
    const std::size_t worst = flat ? static_cast<std::size_t>(count) * 3u
        : primitive == 6u ? static_cast<std::size_t>(count) * 2u : vertex_count;
    if (g_batch_count + worst > 65535u) {
        flush();
        g_batch_vtype = vtype;
        g_layout = batch_layout(vtype);
        g_batch_min_alpha = 255u;
        g_batch_world = world_applied;
    }
    if (!g_keep_discard) {
        // A lower bound of what the PSP takes as the colour's alpha: the
        // vertex colours when there are any, and the material ambient alpha
        // (unlit without colours, or lit) either way.
        std::uint32_t lowest = cmd(0x58u) & 0xFFu;
        const std::uint32_t col = (vtype >> 2u) & 7u;
        if (col >= 5u && vertex_count != 0u) {
            const std::uint8_t *at = raw + layout.col;
            for (std::uint32_t i = 0; i < vertex_count && lowest != 0u; ++i, at += vertex_stride) {
                std::uint32_t a;
                if (col == 7u) a = at[3];
                else if (col == 6u) a = (at[1] >> 4u) * 17u;
                else a = (at[1] & 0x80u) != 0u ? 255u : 0u;
                lowest = std::min(lowest, a);
            }
        }
        g_batch_min_alpha = std::min(g_batch_min_alpha, lowest);
    }
    const BatchLayout &out = g_layout;
    const auto vertex = [&](std::uint32_t i) { return raw + static_cast<std::size_t>(i) * vertex_stride; };
    // Vertices already in the batch layout, in a snapshot or in static guest
    // memory, are drawn where they are as long as they follow the batch's
    // previous ones. A batch that stops following goes on as a copy.
    const bool placeable = weights == 0u && !world_applied && out.same_as_psp && !flat && primitive != 6u &&
                           (g_snapshot != nullptr || static_vertices(g_vertex_address));
    const bool follows = g_batch_in_place != nullptr && raw == g_batch_in_place + static_cast<std::size_t>(g_batch_count) * vertex_stride;
    const bool in_place = placeable && (g_batch_count == 0u || follows);
    if (g_batch_in_place != nullptr && !in_place) {
        const std::uint8_t *placed = g_batch_in_place;
        const std::uint32_t placed_count = g_batch_count;
        g_batch_in_place = nullptr;
        g_batch_count = 0u;
        g_batch.clear();
        g_gpu.copied += static_cast<std::size_t>(placed_count) * vertex_stride;
        std::memcpy(append_vertices(placed_count), placed, static_cast<std::size_t>(placed_count) * vertex_stride);
    }
    if (in_place && g_batch_count == 0u) g_batch_in_place = raw;
    const auto base = g_batch_count;
    // Lists, strips and fans of consecutive vertices only go down as a run.
    const bool generated = index == 0u && !flat && primitive >= 3u && primitive <= 5u;
    std::size_t index_start = g_batch_indices.size();
    if (!generated) g_batch_indices.resize(index_start + static_cast<std::size_t>(count) * 3u);
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
        if (out.same_as_psp) std::memcpy(dst, raw, static_cast<std::size_t>(vertex_count) * vertex_stride);
        else copy_vertices(dst, raw, vertex_count, out, layout);
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
    if (generated) {
        const std::uint32_t triangles = primitive == 3u ? count / 3u : count >= 3u ? count - 2u : 0u;
        g_batch_runs.push_back(base);
        g_batch_runs.push_back(count | primitive << 24u);
        g_batch_index_total += triangles * 3u;
        if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
        else g_vertex_address += count * stride;
        phase = 0u;
        return true;
    }
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
    const auto written = static_cast<std::uint32_t>(next - (g_batch_indices.data() + index_start));
    g_batch_indices.resize(index_start + written);
    g_batch_runs.push_back(static_cast<std::uint32_t>(index_start));
    g_batch_runs.push_back(written | 7u << 24u);
    g_batch_index_total += written;
    if (index != 0u) g_index_address += count * (index == 1u ? 1u : 2u);
    else g_vertex_address += count * stride;
    phase = 0u;
    return true;
}

// Commands that do not change how the pending batch is drawn.
bool batch_neutral(std::uint32_t op) noexcept {
    switch (op) {
    case 0x00u: case 0x01u: case 0x02u: case 0x03u: case 0x04u: case 0x08u: case 0x0Au: case 0x0Bu: case 0x0Cu:
    case 0x0Eu: case 0x0Fu: case 0x10u: case 0x13u: case 0x14u:
    case 0x2Au: case 0x2Bu:  // bones: skinning is applied as vertices are decoded
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- GL thread side

struct GlThread {
    SyncMutex mutex{"ge_gl"};  // finish() may run on the SCE main thread
    SyncCondition cv{"ge_gl", mutex};
    std::deque<Frame> frames;
    // Op and data buffers of replayed frames, emptied, for the next frames.
    std::vector<std::pair<std::vector<std::uint32_t>, std::vector<std::uint8_t>>> spares;
    bool busy{}, ready{}, ok{};
    std::string error;
} g_gl;

std::vector<GLuint> g_gl_textures, g_gl_framebuffers;  // GL names by id (GL thread)
// Render targets are drawn at this multiple of the PSP resolution, in
// eighths (8 = 1x, 12 = 1.5x, 14 = 1.75x, 16 = 2x). Only the GL thread applies it: targets,
// viewports and scissors are recorded in PSP pixels and the shader works in
// normalized coordinates.
int g_scale{8};
int scaled(int value, int scale) noexcept { return value * scale / 8; }
// Present: sharpen the picture as it is enlarged to the screen (below 2x).
bool g_present_sharpen{std::getenv("PSPRECOMP_BURNOUT_SHARPEN") != nullptr};
std::atomic<double> g_gl_busy_ms{0.0}, g_gl_present_ms{0.0};

// Once per frame, as its present is recorded: the next frame's resolution.
void update_dynamic_resolution() {
    if (!g_dynamic_resolution) return;
    static double last_present = 0.0, smooth = 0.0;
    static int calm = 0, hold = 0, ceiling = 0, ceiling_frames = 0;
    static std::uint64_t at_level[33]{};
    static std::uint64_t frames = 0;
    if (g_dynamic_q == 0) g_dynamic_q = ceiling = g_scale;
    const double total = g_gl_present_ms.load();
    const double wait = total - last_present;
    last_present = total;
    smooth = smooth * 0.75 + wait * 0.25;
    ++at_level[g_dynamic_q];
    ++frames;
    if (ceiling_frames > 0 && --ceiling_frames == 0) ceiling = g_scale;
    static const bool test = std::getenv("PSPRECOMP_BURNOUT_DYNAMIC_TEST") != nullptr;
    if (test) {
        // PC check: every level in turn, 60 frames each.
        g_dynamic_q = g_scale - static_cast<int>((frames / 60u) % static_cast<std::uint64_t>(g_scale - g_dynamic_min + 1));
    } else if (hold > 0) {
        --hold;
    } else if (smooth > 2.0 && g_dynamic_q > g_dynamic_min) {
        // The GPU is late: one step down, and that level stays out of reach
        // for ~10 s so the picture does not pump between two sizes.
        ceiling = g_dynamic_q - 1;
        ceiling_frames = 300;
        --g_dynamic_q;
        hold = 6;
        calm = 0;
    } else if (smooth < 0.6) {
        if (++calm >= 45 && g_dynamic_q < ceiling) {
            ++g_dynamic_q;
            calm = 0;
            hold = 6;
        }
    } else {
        calm = 0;
    }
    if (frames % 300u == 0u) {
        std::string line = "dynamic resolution: now " + std::to_string(480 * g_dynamic_q / 8) + "x" + std::to_string(272 * g_dynamic_q / 8) +
                           ", frames by width:";
        for (int q = g_scale; q >= g_dynamic_min; --q)
            if (at_level[q] != 0u) line += " " + std::to_string(480 * q / 8) + ":" + std::to_string(at_level[q]);
        trace(line.c_str());
        for (auto &n : at_level) n = 0u;
    }
}
// GL thread breakdown (gl-74): draws issued, the vertex/index preparation
// (index expansion and copies to GPU memory) and the draws past the budget.
std::atomic<double> g_gl_upload_ms{0.0}, g_gl_draw_ms{0.0};
std::atomic<std::uint64_t> g_gl_draws{0u}, g_gl_budget_dropped{0u};
std::uint32_t g_draw_budget{[] { const char *v = std::getenv("PSPRECOMP_BURNOUT_DRAW_BUDGET"); return v != nullptr ? static_cast<std::uint32_t>(std::atoi(v)) : 0u; }()};  // test: draws past this in a frame are not drawn (0: all)
std::uint32_t g_gl_frame_draws{};

GLuint gl_texture(GLuint id) {
    if (id >= g_gl_textures.size()) g_gl_textures.resize(id + 1u, 0u);
    if (g_gl_textures[id] == 0u) glGenTextures(1, &g_gl_textures[id]);
    return g_gl_textures[id];
}

// GL thread: scale of each framebuffer id, and of the one bound (scissor and
// viewport are recorded in PSP pixels).
std::vector<int> g_framebuffer_scales;  // allocated, in eighths as g_scale
std::vector<int> g_framebuffer_drawn;   // drawn at (dynamic resolution), 0: allocated
int g_bound_scale{8};

std::vector<GLuint> g_gl_depths;  // depth/stencil renderbuffer by framebuffer id

void create_target(GLuint framebuffer_id, GLuint texture_id, int width, int height) {
    if (framebuffer_id < g_gl_framebuffers.size() && g_gl_framebuffers[framebuffer_id] != 0u) {
        // Recreated at another size (dynamic resolution): the old objects go.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &g_gl_framebuffers[framebuffer_id]);
        g_gl_framebuffers[framebuffer_id] = 0u;
        if (framebuffer_id < g_gl_depths.size() && g_gl_depths[framebuffer_id] != 0u) {
            glDeleteRenderbuffers(1, &g_gl_depths[framebuffer_id]);
            g_gl_depths[framebuffer_id] = 0u;
        }
        if (texture_id < g_gl_textures.size() && g_gl_textures[texture_id] != 0u) {
            glDeleteTextures(1, &g_gl_textures[texture_id]);
            g_gl_textures[texture_id] = 0u;
        }
    }
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
    // Stencil too: the PSP keeps it in the frame buffer's alpha (see flush).
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(0.0);
    glClearStencil(0);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (framebuffer_id >= g_gl_framebuffers.size()) g_gl_framebuffers.resize(framebuffer_id + 1u, 0u);
    g_gl_framebuffers[framebuffer_id] = framebuffer;
    if (framebuffer_id >= g_gl_depths.size()) g_gl_depths.resize(framebuffer_id + 1u, 0u);
    g_gl_depths[framebuffer_id] = depth;
}

// Latest value of each uniform slot, versioned so a program only uploads what
// changed since it last drew.
struct SlotValue {
    std::array<float, 30 * 4> values{};
    std::uint32_t count{}, version{};
    bool matrix{};
};
std::array<SlotValue, kSlotCount> g_slot_values;
std::unordered_map<std::uint32_t, Program *> g_variants;  // by variant key; the uber program when one failed
std::deque<Program> g_variant_programs;
Program *g_bound{};  // program in use; null after present changed it

void set_slot(std::uint32_t slot, const float *values, std::uint32_t count, bool matrix) {
    SlotValue &value = g_slot_values[slot];
    std::memcpy(value.values.data(), values, count * sizeof(float));
    value.count = count;
    value.matrix = matrix;
    ++value.version;
}

// Preprocessor defines of a variant key (bits set in flush and
// upload_vertex_state).
std::string variant_defines(std::uint32_t key) {
    const auto bits = [&](int at, int width) { return (key >> at) & ((1u << width) - 1u); };
    const std::pair<const char *, std::uint32_t> defines[]{
        {"THROUGH", bits(0, 1)},    {"LIT", bits(1, 1)},     {"TEXGEN", bits(2, 2)},  {"SOURCE", bits(4, 2)},
        {"COLFMT", bits(6, 3)},     {"FOG", bits(9, 1)},     {"TEXTURED", bits(10, 1)}, {"TEXFUNC", bits(11, 3)},
        {"TCC", bits(14, 1)},       {"DOUBLE", bits(15, 1)}, {"PROJECTIVE", bits(16, 1)}, {"ATEST", bits(17, 1)},
        {"ATFUNC", bits(18, 3)},    {"BLENDFIX", bits(21, 1)}, {"ALPHAOUT", bits(22, 1)}, {"CTEST", bits(23, 1)},
        {"CTFUNC", bits(24, 2)},    {"ATBLEND", bits(26, 1)},
        {"ATFAST", bits(27, 1)}};
    std::string text;
    for (const auto &[name, value] : defines) text += "#define " + std::string(name) + " " + std::to_string(value) + "\n";
    return text;
}

// Variants met while playing are appended to this file and compiled at the
// next launch with the known ones (empty: not kept).
std::string g_variant_list;
bool g_booting{true};  // compiling the variants at launch

void remember_variant(std::uint32_t key) {
    if (g_booting || g_variant_list.empty()) return;
    if (std::FILE *file = std::fopen(g_variant_list.c_str(), "a")) {
        std::fprintf(file, "%08X\n", static_cast<unsigned>(key));
        std::fclose(file);
    }
}

std::uint32_t g_current_key{};
// GL thread state the probe modes change around a draw.
bool g_gl_depth_mask{true};
GLenum g_gl_depth_func{GL_LESS};
GLboolean g_gl_color_rgb{GL_TRUE}, g_gl_color_alpha{GL_TRUE};
bool g_prepass_next{};  // the next Draw is a cut-out drawn with a depth prepass
GLuint g_gl_bound_texture{};
std::unordered_map<GLuint, std::array<GLint, 2>> g_gl_filters;
bool g_fragment_stats_flush{};
void use_program(std::uint32_t key) {
    g_current_key = key;
    static const bool uber_only = std::getenv("PSPRECOMP_BURNOUT_UBER") != nullptr;
    auto it = g_variants.find(key);
    if (it == g_variants.end()) {
        Program *program = &g_uber;
        const double started = host_now_ms();
        if (!uber_only) {
            Program &variant = g_variant_programs.emplace_back();
            std::string error;
            if (link(variant, kVariantVertexShader, kVariantFragmentShader, false, error, variant_defines(key))) {
                program = &variant;
            } else {
                g_variant_programs.pop_back();
                trace(("program variant failed, using the general shaders: " + error).c_str());
            }
        }
        it = g_variants.emplace(key, program).first;
        if (g_probe_mode.load(std::memory_order_relaxed) == 0u) remember_variant(key);
        char line[96];
        std::snprintf(line, sizeof(line), "program variant %06X (%u so far) in %.0f ms", static_cast<unsigned>(key),
                      static_cast<unsigned>(g_variants.size()), host_now_ms() - started);
        trace(line);
    }
    if (g_bound != it->second) {
        g_bound = it->second;
        glUseProgram(g_bound->id);
    }
}

// Variants the races use, compiled while the game boots rather than on
// first use in a race (each costs the Vita's shader compiler a moment).
constexpr std::uint32_t kKnownVariants[]{
    0x000000u, 0x000001u, 0x000002u, 0x000340u, 0x00040Du, 0x004400u, 0x00440Du, 0x004740u,
    0x005C00u, 0x005C02u, 0x00C602u, 0x01C626u, 0x024400u, 0x1A0000u, 0x1A4400u, 0x1A4540u,
    0x1A4740u, 0x1E440Du, 0x1E4540u, 0x21C626u, 0x62C602u, 0x7AC602u, 0x380040Du, 0x404740u, 0x005E00u, 0x000200u,
    // Met in free play on the US tracks (gl-47 log, 0.6-1.7 s each mid-race).
    0x605C00u, 0x600000u, 0x605C02u, 0x600002u, 0x7EC602u, 0x1E4400u, 0x7E5C00u, 0x7E0000u};

void compile_remembered_variants() {
    if (g_variant_list.empty()) return;
    std::FILE *file = std::fopen(g_variant_list.c_str(), "r");
    if (file == nullptr) return;
    unsigned key = 0u;
    while (std::fscanf(file, "%x", &key) == 1) use_program(key);
    std::fclose(file);
}

void sync_uniforms() {
    if (g_bound == nullptr) use_program(0u);
    Program &program = *g_bound;
    for (int i = 0; i < kSlotCount; ++i) {
        const GLint location = program.slots[i];
        const SlotValue &value = g_slot_values[i];
        if (location < 0 || program.versions[i] == value.version) continue;
        program.versions[i] = value.version;
        if (value.matrix) glUniformMatrix4fv(location, 1, GL_FALSE, value.values.data());
        else glUniform4fv(location, static_cast<GLsizei>(value.count / 4u), value.values.data());
    }
}

// Triangle indices of a batch from its runs (see g_batch_runs).
void expand_indices(std::uint16_t *out, const std::uint32_t *runs, std::uint32_t run_words, const std::uint16_t *explicit_indices) {
    for (std::uint32_t r = 0; r + 1u < run_words; r += 2u) {
        const std::uint32_t first = runs[r], count = runs[r + 1u] & 0xFFFFFFu, kind = runs[r + 1u] >> 24u;
        switch (kind) {
        case 3u:
            for (std::uint32_t i = 0; i + 2u < count; i += 3u) {
                out[0] = static_cast<std::uint16_t>(first + i);
                out[1] = static_cast<std::uint16_t>(first + i + 1u);
                out[2] = static_cast<std::uint16_t>(first + i + 2u);
                out += 3;
            }
            break;
        case 4u:
            for (std::uint32_t i = 0; i + 2u < count; ++i) {
                out[0] = static_cast<std::uint16_t>(first + i + (i & 1u));
                out[1] = static_cast<std::uint16_t>(first + i + 1u - (i & 1u));
                out[2] = static_cast<std::uint16_t>(first + i + 2u);
                out += 3;
            }
            break;
        case 5u:
            for (std::uint32_t i = 1; i + 1u < count; ++i) {
                out[0] = static_cast<std::uint16_t>(first);
                out[1] = static_cast<std::uint16_t>(first + i);
                out[2] = static_cast<std::uint16_t>(first + i + 1u);
                out += 3;
            }
            break;
        default:
            std::memcpy(out, explicit_indices + first, count * sizeof(std::uint16_t));
            out += count;
            break;
        }
    }
}

void draw_body(const Frame &frame, const std::uint32_t *a, double started);
void draw(const Frame &frame, const std::uint32_t *a) {
    if (g_draw_budget != 0u && g_gl_frame_draws >= g_draw_budget) {
        g_gl_budget_dropped.store(g_gl_budget_dropped.load() + 1u);
        return;
    }
    ++g_gl_frame_draws;
    g_gl_draws.store(g_gl_draws.load() + 1u);
    const double started = host_now_ms();
    draw_body(frame, a, started);
    g_gl_draw_ms.store(g_gl_draw_ms.load() + host_now_ms() - started);
}
void draw_body(const Frame &frame, const std::uint32_t *a, double started) {
    const std::uint32_t in_frame = a[0], source = a[1], bytes = a[2], stride = a[3], attribs_at = a[4];
    const std::uint32_t runs_at = a[5], run_words = a[6], explicit_at = a[7], index_count = a[8];
    const std::uint8_t *vertices = frame.data.data() + source;
    if (in_frame == 0u) {
        std::uintptr_t pointer;
        std::memcpy(&pointer, vertices, sizeof(pointer));
        vertices = reinterpret_cast<const std::uint8_t *>(pointer);
    }
    std::array<AttribSetup, 4> attribs;
    std::memcpy(attribs.data(), frame.data.data() + attribs_at, sizeof(attribs));
    static std::vector<std::uint16_t> expanded;
    expanded.resize(index_count);
    expand_indices(expanded.data(), reinterpret_cast<const std::uint32_t *>(frame.data.data() + runs_at), run_words,
                   reinterpret_cast<const std::uint16_t *>(frame.data.data() + explicit_at));
    const void *indices = expanded.data();
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
    g_gl_upload_ms.store(g_gl_upload_ms.load() + host_now_ms() - started);
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
    sync_uniforms();
    if (g_prepass_next) {
        g_prepass_next = false;
        // Depth only, with the alpha test's discard...
        const std::uint32_t key = g_current_key;
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
        glColorMask(g_gl_color_rgb, g_gl_color_rgb, g_gl_color_rgb, g_gl_color_alpha);
        // ...then the colour where that depth is (same vertex shader: the
        // same depths), by the variant without the alpha test.
        glDepthFunc(GL_EQUAL);
        glDepthMask(GL_FALSE);
        use_program(key & ~(1u << 17u | 7u << 18u | 1u << 27u));
        sync_uniforms();
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
        glDepthFunc(g_gl_depth_func);
        glDepthMask(g_gl_depth_mask ? GL_TRUE : GL_FALSE);
        use_program(key);
        return;
    }
    if (g_gpu_probe && (g_current_key >> 17u & 1u) != 0u && (g_current_key >> 10u & 1u) != 0u) {
        // Alpha-tested textured draws: 2 nearest filtering, 3 drawn as two
        // halves (twice the draws, same pixels), 4 no depth writes.
        const std::uint32_t mode = g_probe_mode.load(std::memory_order_relaxed);
        if (mode == 2u) {
            const auto it = g_gl_filters.find(g_gl_bound_texture);
            const std::array<GLint, 2> filters =
                it != g_gl_filters.end() ? it->second : std::array<GLint, 2>{GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR};
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filters[0]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filters[1]);
            return;
        }
        if (mode == 3u && index_count >= 6u) {
            const std::uint32_t first = index_count / 6u * 3u;
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(first), GL_UNSIGNED_SHORT, indices);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count - first), GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void *>(reinterpret_cast<std::uintptr_t>(indices) + first * 2u));
            return;
        }
        if (mode == 4u && g_gl_depth_mask) {
            glDepthMask(GL_FALSE);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
            glDepthMask(GL_TRUE);
            return;
        }
    }
#if !defined(__vita__)
    // Diagnostic (PC): fragment shader invocations per kind of draw, i.e. the
    // fragment work a tile GPU without early rejection would do.
    static const bool fragment_stats = std::getenv("PSPRECOMP_BURNOUT_FRAG_STATS") != nullptr;
    if (fragment_stats) {
        static std::vector<GLuint> pool;
        static std::vector<std::uint32_t> kinds;
        static std::size_t used = 0;
        static double totals[4]{}, draws[4]{};
        static std::uint32_t frames = 0;
        if (used == pool.size()) {
            pool.resize(pool.size() + 64u);
            glGenQueries(64, pool.data() + used);
            kinds.resize(pool.size());
        }
        const bool atest = (g_current_key >> 17u & 1u) != 0u;
        const bool textured = (g_current_key >> 10u & 1u) != 0u;
        kinds[used] = atest ? (textured ? 0u : 1u) : (textured ? 2u : 3u);
        glBeginQuery(0x82F4 /* GL_FRAGMENT_SHADER_INVOCATIONS_ARB */, pool[used]);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
        glEndQuery(0x82F4);
        ++used;
        if (used >= 4096u || g_fragment_stats_flush) {
            for (std::size_t i = 0; i < used; ++i) {
                unsigned long long value = 0;
                glGetQueryObjectui64v(pool[i], 0x8866 /* GL_QUERY_RESULT */, &value);
                totals[kinds[i]] += static_cast<double>(value);
                draws[kinds[i]] += 1.0;
            }
            used = 0;
        }
        if (g_fragment_stats_flush) {
            g_fragment_stats_flush = false;
            if (++frames % 300u == 0u) {
                const double px = 1024.0 * 544.0 * 300.0;
                std::fprintf(stderr,
                             "[frag] per frame, in screens of 1024x544: alpha-tested textured %.2f (%.0f draws), alpha-tested "
                             "untextured %.2f (%.0f), textured %.2f (%.0f), untextured %.2f (%.0f)\n",
                             totals[0] / px, draws[0] / 300.0, totals[1] / px, draws[1] / 300.0, totals[2] / px,
                             draws[2] / 300.0, totals[3] / px, draws[3] / 300.0);
                for (int i = 0; i < 4; ++i) totals[i] = draws[i] = 0.0;
            }
        }
        return;
    }
#endif
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_SHORT, indices);
}

void present_target(GLuint texture_id, int width, float fraction) {
#if defined(__vita__)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (texture_id != 0u) {
        const float u = 480.0f / static_cast<float>(width), v = 1.0f;
        const float quad[16]{-1.0f, 1.0f, 0.0f, v, 1.0f, 1.0f, u, v, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, u, 0.0f};
        // Sharpened only when the picture is enlarged.
        const Program &present = fraction * static_cast<float>(g_scale) < 15.5f && g_present_sharp.id != 0u ? g_present_sharp : g_present;
        glUseProgram(present.id);
        g_bound = nullptr;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_texture(texture_id));
        glUniform1i(present.texture, 0);
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
    (void)fraction;
#endif
}

void capture_target(GLuint framebuffer_id, int height, const std::function<void(std::vector<std::uint8_t> &)> &done) {
    std::vector<std::uint8_t> rgb;
    if (framebuffer_id < g_gl_framebuffers.size() && g_gl_framebuffers[framebuffer_id] != 0u) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_gl_framebuffers[framebuffer_id]);
        // The visible 480x272 at the render scale, averaged back to PSP size.
        int scale = framebuffer_id < g_framebuffer_scales.size() ? g_framebuffer_scales[framebuffer_id] : g_scale;
        if (framebuffer_id < g_framebuffer_drawn.size() && g_framebuffer_drawn[framebuffer_id] != 0) scale = g_framebuffer_drawn[framebuffer_id];
        // (scale in eighths: each PSP pixel is the box [x * s / 8, (x + 1) * s / 8).)
        const auto s = static_cast<std::uint32_t>(scale);
        const std::uint32_t w = 480u * s / 8u, h = 272u * s / 8u;
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
        glReadPixels(0, scaled(height - 272, scale), static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba.data());
        rgb.resize(480u * 272u * 3u);
        for (std::uint32_t y = 0; y < 272u; ++y) {
            const std::uint32_t fy = 271u - y, y0 = fy * s / 8u, y1 = (fy + 1u) * s / 8u;
            for (std::uint32_t x = 0; x < 480u; ++x) {
                const std::uint32_t x0 = x * s / 8u, x1 = (x + 1u) * s / 8u;
                for (int c = 0; c < 3; ++c) {
                    std::uint32_t sum = 0u;
                    for (std::uint32_t sy = y0; sy < y1; ++sy)
                        for (std::uint32_t sx = x0; sx < x1; ++sx) sum += rgba[(sy * w + sx) * 4u + c];
                    rgb[(y * 480u + x) * 3u + c] = static_cast<std::uint8_t>(sum / ((y1 - y0) * (x1 - x0)));
                }
            }
        }
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
        case Op::DepthFunc:
            g_gl_depth_func = op[0];
            glDepthFunc(op[0]);
            op += 1;
            break;
        case Op::Prepass: g_prepass_next = op[0] != 0u; op += 1; break;
        case Op::DepthMask:
            g_gl_depth_mask = op[0] != 0u;
            glDepthMask(op[0] != 0u ? GL_TRUE : GL_FALSE);
            op += 1;
            break;
        case Op::ColorMask: {
            const GLboolean rgb = op[0] != 0u ? GL_TRUE : GL_FALSE;
            g_gl_color_rgb = rgb;
            g_gl_color_alpha = op[1] != 0u ? GL_TRUE : GL_FALSE;
            glColorMask(rgb, rgb, rgb, g_gl_color_alpha);
            op += 2;
            break;
        }
        case Op::FrontFace: glFrontFace(op[0]); op += 1; break;
        case Op::StencilFunc: glStencilFunc(op[0], static_cast<GLint>(op[1]), op[2]); op += 3; break;
        case Op::StencilOp: glStencilOp(op[0], op[1], op[2]); op += 3; break;
        case Op::StencilMask: glStencilMask(op[0]); op += 1; break;
        case Op::Scissor: {
            const GLsizei cut = 1;
            const int x = static_cast<int>(op[0]), y = static_cast<int>(op[1]);
            const int x0 = scaled(x, g_bound_scale), y0 = scaled(y, g_bound_scale);
            glScissor(x0, y0, (scaled(x + static_cast<int>(op[2]), g_bound_scale) - x0) / cut,
                      (scaled(y + static_cast<int>(op[3]), g_bound_scale) - y0) / cut);
        }
            op += 4;
            break;
        case Op::Viewport:
            glViewport(0, 0, scaled(static_cast<int>(op[0]), g_bound_scale), scaled(static_cast<int>(op[1]), g_bound_scale));
            op += 2;
            break;
        case Op::UseProgram: use_program(probe_key(op[0])); op += 1; break;
        case Op::BindFramebuffer:
            glBindFramebuffer(GL_FRAMEBUFFER, op[0] < g_gl_framebuffers.size() ? g_gl_framebuffers[op[0]] : 0u);
            g_bound_scale = op[1] != 0u ? static_cast<int>(op[1])
                            : op[0] < g_framebuffer_scales.size() ? g_framebuffer_scales[op[0]] : g_scale;
            if (op[0] >= g_framebuffer_drawn.size()) g_framebuffer_drawn.resize(op[0] + 1u, 0);
            g_framebuffer_drawn[op[0]] = static_cast<int>(op[1]);
            op += 2;
            break;
        case Op::BindTexture:
            g_gl_bound_texture = gl_texture(op[0]);
            glBindTexture(GL_TEXTURE_2D, g_gl_bound_texture);
            op += 1;
            break;
        case Op::TexParameter:
            if (g_gpu_probe) {
                auto &filters = g_gl_filters[g_gl_bound_texture];
                if (op[0] == GL_TEXTURE_MIN_FILTER) filters[0] = static_cast<GLint>(op[1]);
                if (op[0] == GL_TEXTURE_MAG_FILTER) filters[1] = static_cast<GLint>(op[1]);
            }
            glTexParameteri(GL_TEXTURE_2D, op[0], static_cast<GLint>(op[1]));
            op += 2;
            break;
        case Op::TexImage:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(op[0]), static_cast<GLsizei>(op[1]), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, frame.blobs[op[2]].data());
            if (op[3] != 0u) glGenerateMipmap(GL_TEXTURE_2D);
            op += 4;
            break;
        case Op::TexImageP8:
#if defined(__vita__)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX8_EXT, static_cast<GLsizei>(op[0]), static_cast<GLsizei>(op[1]), 0,
                         GL_COLOR_INDEX, GL_UNSIGNED_BYTE, frame.blobs[op[2]].data());
            glColorTable(GL_TEXTURE_2D, GL_RGBA, 256, GL_RGBA, GL_UNSIGNED_BYTE, frame.blobs[op[2] + 1u].data());
#endif
            op += 3;
            break;
        case Op::Uniform4: set_slot(op[0], floats(op[2]), op[1] * 4u, false); op += 3; break;
        case Op::UniformMatrix: set_slot(op[0], floats(op[1]), 16u, true); op += 2; break;
        case Op::Draw: draw(frame, op); op += 9; break;
        case Op::CreateTarget: {
            const int scale = static_cast<int>(op[4]);
            create_target(op[0], op[1], scaled(static_cast<int>(op[2]), scale), scaled(static_cast<int>(op[3]), scale));
            if (op[0] >= g_framebuffer_scales.size()) g_framebuffer_scales.resize(op[0] + 1u, g_scale);
            g_framebuffer_scales[op[0]] = scale;
            g_bound_scale = scale;  // create_target leaves it bound
            op += 5;
            break;
        }
        case Op::Present: {
            g_gl_frame_draws = 0u;
            const double started = host_now_ms();
            present_target(op[0], static_cast<int>(op[1]), static_cast<float>(op[2]) / static_cast<float>(g_scale));
            g_fragment_stats_flush = true;
            g_gl_present_ms.store(g_gl_present_ms.load() + host_now_ms() - started);
            op += 3;
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
        ok = ok && link(g_uber, kVertexShader, kFragmentShader, false, error);
        if (ok) trace("compiling the present shaders");
        ok = ok && link(g_present, kPresentVertexShader, kPresentFragmentShader, true, error, "#define SHARPEN 0\n");
        if (ok && g_present_sharpen && (g_scale < 16 || g_dynamic_resolution)) {
            // The display target is 512x272 PSP pixels at the render scale.
            char text[160];
            std::snprintf(text, sizeof(text), "#define SHARPEN 1\n#define TEXEL_X %.8f\n#define TEXEL_Y %.8f\n#define STRENGTH 0.5\n",
                          8.0 / (512.0 * g_scale), 8.0 / (272.0 * g_scale));
            ok = link(g_present_sharp, kPresentVertexShader, kPresentFragmentShader, true, error, text);
        }
        if (ok) {
            trace("compiling the known program variants");
            for (const std::uint32_t key : kKnownVariants) use_program(key);
            compile_remembered_variants();
            g_booting = false;
            g_bound = nullptr;
        }
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
        std::vector<std::uint32_t> spare_ops = std::move(frame.ops);
        std::vector<std::uint8_t> spare_data = std::move(frame.data);
        spare_ops.clear();
        spare_data.clear();
        frame = Frame{};  // release snapshots and payloads before reporting idle
        std::lock_guard lock(g_gl.mutex);
        if (g_gl.spares.size() < 3u) g_gl.spares.emplace_back(std::move(spare_ops), std::move(spare_data));
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
    splice_deferred();
    std::unique_lock lock(g_gl.mutex);
    g_gl.cv.wait(lock, [] { return g_gl.frames.size() < 2u; });
    // The next frame starts with the room this one needed, so recording it
    // does not regrow (and copy) its buffers from nothing.
    const std::size_t ops = g_frame.ops.size(), data = g_frame.data.size();
    g_gl.frames.push_back(std::move(g_frame));
    g_frame = Frame{};
    if (!g_gl.spares.empty()) {
        g_frame.ops = std::move(g_gl.spares.back().first);
        g_frame.data = std::move(g_gl.spares.back().second);
        g_gl.spares.pop_back();
    }
    g_gl.cv.notify_all();
    lock.unlock();
    g_frame.ops.reserve(ops + ops / 4u);
    g_frame.data.reserve(data + data / 4u);
}

} // namespace

namespace {
SyncMutex g_trace_mutex{"gl_trace"};
std::string g_trace_queue;
bool g_trace_queued{};  // set once the renderer is up (until then a hang must leave its trace)
} // namespace

void trace(const char *message) {
#if defined(__vita__)
    if (g_trace_queued) {
        std::lock_guard lock(g_trace_mutex);
        g_trace_queue += "[gl] ";
        g_trace_queue += message;
        g_trace_queue += '\n';
        return;
    }
#endif
    std::fprintf(stderr, "[gl] %s\n", message);
    std::fflush(stderr);
}

void flush_traces() {
    std::string lines;
    {
        std::lock_guard lock(g_trace_mutex);
        lines.swap(g_trace_queue);
    }
    if (lines.empty()) return;
    std::fwrite(lines.data(), 1u, lines.size(), stderr);
    std::fflush(stderr);
}

void set_render_scale(float scale) { g_scale = std::clamp(static_cast<int>(std::lround(scale * 8.0f)), 8, 32); }
void set_dynamic_resolution(bool enabled, float min_scale) {
    g_dynamic_resolution = g_dynamic_resolution || enabled;
    g_dynamic_min = std::clamp(static_cast<int>(std::lround(min_scale * 8.0f)), 8, 32);
}
void set_present_sharpen(bool enabled) { g_present_sharpen = g_present_sharpen || enabled; }
void set_thin_foliage(bool enabled, float near_distance, float far_distance) {
    if (!enabled) return;
    g_thin_foliage = true;
    g_thin_near = near_distance;
    g_thin_far = far_distance;
}
void set_offscreen_1x(bool enabled) { g_offscreen_1x = enabled; }
void set_stencil_ab(bool enabled) { g_stencil_ab = enabled; }
void set_gpu_probe(bool enabled) { g_gpu_probe = enabled; }
void set_fragment_mediump(bool enabled) { g_fragment_mediump = enabled; }
void set_merge_cutouts(bool enabled, bool all) {
    g_merge_cutouts = g_merge_cutouts || enabled;
    g_merge_all = g_merge_all || all;
}
void set_reorder_cutouts(bool enabled) { g_reorder_cutouts = g_reorder_cutouts || enabled; }
void set_blend_deferred_cutouts(bool enabled) { g_blend_deferred_cutouts = g_blend_deferred_cutouts || enabled; }
void set_no_trees(bool enabled) { g_no_cutouts = g_no_cutouts || enabled; }
void set_cutout_prepass(bool enabled) { g_cutout_prepass = g_cutout_prepass || enabled; }
void set_draw_budget(unsigned budget) { g_draw_budget = budget; }
void set_shader_cache(const std::string &directory) { g_shader_cache = directory; }
void set_variant_list(const std::string &path) { g_variant_list = path; }
void set_cached_memory(bool cached) { g_cached_memory = cached; }
bool cached_memory() noexcept { return g_cached_memory; }

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
    g_trace_queued = true;
    g_commands.fill(0u);
    return true;
}

void finish() {
    if (!g_frame.ops.empty()) submit_frame();
    {
        std::unique_lock lock(g_gl.mutex);
        g_gl.cv.wait(lock, [] { return g_gl.frames.empty() && !g_gl.busy; });
    }
    flush_traces();
}

// Commands that act even when they repeat the register's current word.
constexpr std::array<bool, 256> kActsWhenRepeated = [] {
    std::array<bool, 256> acts{};
    for (const std::uint32_t op : {0x01u, 0x02u, 0x04u, 0x0Cu, 0x2Au, 0x2Bu, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu,
                                   0x40u, 0x41u, 0xC4u, 0xEAu})
        acts[op] = true;
    return acts;
}();

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset) {
    const std::uint32_t op = word >> 24u;
    // Most of a list re-sends state that is already set: nothing to do.
    if (g_commands[op] == word && !kActsWhenRepeated[op]) return;
    g_memory = &memory;
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
        default: return false;
        }
    };
    // (0x12 vertex type: decode_prim ends the batch if the type differs; 0x07/0x09 bounding box test and jump.)
    const bool world_only = (op == 0x3Au || op == 0x3Bu || op == 0x12u || op == 0x07u || op == 0x09u) && g_batch_world && g_batch_count != 0u;
    if (!world_only && !batch_neutral(op) && (matrix_changes() || g_commands[op] != word || op == 0xC4u || op == 0xEAu)) {
        static const bool merge_stats = std::getenv("PSPRECOMP_BURNOUT_MERGE_STATS") != nullptr;
        if (merge_stats && g_batch_world && g_batch_count != 0u) {
            static std::array<std::uint64_t, 256> by_op{};
            static std::uint64_t n = 0;
            ++by_op[op];
            if (++n % 20000u == 0u) {
                std::string line = "[merge] world batches ended by:";
                for (std::uint32_t i = 0; i < 256u; ++i)
                    if (by_op[i] * 50u >= n) line += " " + std::to_string(i) + "=" + std::to_string(by_op[i] * 100u / n) + "%";
                std::fprintf(stderr, "%s\n", line.c_str());
            }
        }
        flush();
    }
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
    case 0x2Au: g_bone_cursor = data & 0x7Fu; break;
    case 0x2Bu: if (g_bone_cursor < g_bones.size()) g_bones[g_bone_cursor++] = float24(data); break;
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
        // The loaded entries only (16-colour palettes are 32-64 bytes): the
        // whole 1 KiB took ~1000 cycles a load on the Vita.
        g_clut_hash = fnv(g_clut.data(), bytes, 0xCBF29CE484222325ull ^ bytes);
        break;
    }
    default: break;
    }
}

SnapshotState g_scan;

namespace {

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
    if (op == 0x04u) {
        // Static vertices drawn in order need no record (see prim_has_record).
        static std::uint32_t layout_vtype = UINT32_MAX, layout_stride = 0u;
        if (g_scan.vtype != layout_vtype) {
            layout_vtype = g_scan.vtype;
            layout_stride = vertex_layout(g_scan.vtype).stride;
        }
        if (((g_scan.vtype >> 11u) & 3u) == 0u && static_vertices(g_scan.vertex_address)) {
            g_scan.vertex_address += (data & 0xFFFFu) * layout_stride;
            return UINT32_MAX;
        }
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

void ge_run(psprecomp::GuestMemory &memory, const std::uint32_t *words, std::uint32_t count, std::uint32_t base,
            std::uint32_t offset, const Snapshot &snapshot, const std::uint32_t *records, std::size_t &next_record) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t word = words[i];
        const std::uint32_t op = word >> 24u;
        if (op == 0xC4u || (op == 0x04u && prim_has_record())) {
            use_snapshot(&snapshot, records[next_record++]);
            ge_command(memory, word, base, offset);
            use_snapshot(nullptr, 0u);
        } else if (g_commands[op] != word || kActsWhenRepeated[op]) {
            // Repeated state (most of a list) is dropped here, without a call.
            ge_command(memory, word, base, offset);
        }
    }
}

bool prim_has_record() noexcept {
    return ((cmd(0x12u) >> 11u) & 3u) != 0u || !static_vertices(g_vertex_address);
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
    g_sun_box.valid = false;
    g_sun_copy = UINT32_MAX;
    flush();
    splice_deferred();
    phase = 11u;
    ++g_stats.frames;
    if (g_gpu_probe) g_probe_mode.store(static_cast<std::uint32_t>((g_stats.frames / 600u) % 6u), std::memory_order_relaxed);
    static const char *fixed_probe = std::getenv("PSPRECOMP_BURNOUT_PROBE_MODE");
    if (fixed_probe != nullptr) {
        g_gpu_probe = true;
        g_probe_mode.store(static_cast<std::uint32_t>(std::atoi(fixed_probe)), std::memory_order_relaxed);
    }
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
        char line[512];
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
        static double last_upload = 0.0, last_draw = 0.0;
        static std::uint64_t last_draws = 0u, last_dropped = 0u;
        const double busy = (g_gl_busy_ms.load() - last_busy) / n, present = (g_gl_present_ms.load() - last_present) / n;
        const double upload = (g_gl_upload_ms.load() - last_upload) / n, draws_ms = (g_gl_draw_ms.load() - last_draw) / n;
        std::snprintf(line, sizeof(line), "  GL thread: %.0f draws/frame (%.0f past budget %u), present %.1f, vertex+index upload %.1f, draw calls and state %.1f, other ops %.1f ms/frame",
                      static_cast<double>(g_gl_draws.load() - last_draws) / n, static_cast<double>(g_gl_budget_dropped.load() - last_dropped) / n,
                      g_draw_budget, present, upload, draws_ms - upload, busy - present - draws_ms);
        trace(line);
        last_upload = g_gl_upload_ms.load();
        last_draw = g_gl_draw_ms.load();
        last_draws = g_gl_draws.load();
        last_dropped = g_gl_budget_dropped.load();
        std::snprintf(line, sizeof(line), "  GL thread busy %.1f ms/frame; textures hashed %.0f KiB/frame; GPU probe mode %d",
                      busy,
                      static_cast<double>(g_stats.texture_bytes_hashed - last.texture_bytes_hashed) / n / 1024.0,
                      g_gpu_probe ? static_cast<int>(((g_stats.frames - 1u) / 600u) % 6u) : -1);
        trace(line);
        // GPU work per frame, and the busiest render targets.
        std::snprintf(line, sizeof(line),
                      "gpu: %.0f prepassed, %.1f target switches, %.0f draws (%.0f blended, %.0f alpha/colour test, %.0f tests dropped, %.0f as blending, %.0f deferred in %.0f runs, %.0f 2D), %.1f target samples, %.0f textured (%.0f with mip levels, %.0f mip filter), %.0f skinned vertices (%.0f weights, %.0f calls, %.0f reused, %.2f ms), %.0f KiB re-copied per frame",
                      static_cast<double>(g_prepassed) / n, g_gpu.switches / n, g_gpu.draws / n, g_gpu.blended / n, g_gpu.discards / n, g_gpu.discards_dropped / n, g_gpu.alpha_blended / n, static_cast<double>(g_deferred_draws) / n, static_cast<double>(g_deferred_splices) / n, g_gpu.through / n,
                      g_gpu.samples / n, g_gpu.textured / n, g_gpu.mipmapped / n, g_gpu.minmip / n, g_gpu.skinned / n, g_gpu.weights / n, g_gpu.skin_calls / n, g_gpu.skin_hits / n, g_gpu.skin_ms / n, g_gpu.copied / n / 1024.0);
        trace(line);
        std::vector<std::pair<std::uint32_t, TargetUse>> used(g_gpu.targets.begin(), g_gpu.targets.end());
        std::sort(used.begin(), used.end(), [](const auto &a, const auto &b) { return a.second.draws > b.second.draws; });
        std::string targets = "gpu targets (key width: draws/binds/samples/blended/2D per frame):";
        for (std::size_t i = 0; i < used.size() && i < 6u; ++i) {
            const TargetUse &u = used[i].second;
            char item[96];
            std::snprintf(item, sizeof(item), " %06X %d: %.0f/%.1f/%.1f/%.0f/%.0f", used[i].first, u.width, u.draws / n,
                          u.binds / n, u.sampled / n, u.blended / n, u.through / n);
            targets += item;
        }
        trace(targets.c_str());
        g_gpu = GpuCounters{};
        g_prepassed = 0u;
        g_deferred_draws = 0u;
        g_deferred_splices = 0u;
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
                         it != g_targets.end() ? static_cast<std::uint32_t>(it->second.width) : 0u,
                         it != g_targets.end() ? static_cast<std::uint32_t>(it->second.draw_q) : static_cast<std::uint32_t>(g_scale)});
    update_dynamic_resolution();
    forget_state();
    submit_frame();
    phase = 0u;
}

void capture(std::uint32_t address, std::function<void(std::vector<std::uint8_t> &rgb)> done) {
    flush();
    if (std::getenv("PSPRECOMP_BURNOUT_TARGET_TRACE") != nullptr)
        std::fprintf(stderr, "[tt] capture after frame %llu\n", static_cast<unsigned long long>(g_stats.frames));
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
