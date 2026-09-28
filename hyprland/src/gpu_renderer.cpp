#include "luxaxis/gpu_renderer.hpp"

#include "luxaxis/spotlight.hpp"
#include "luxaxis/transition.hpp"

#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace luxaxis::gpu {
namespace {

class GlState {
  public:
    GlState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer_);
        glGetIntegerv(GL_VIEWPORT, viewport_.data());
        glGetIntegerv(GL_SCISSOR_BOX, scissor_.data());
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment_);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer_);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength_);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows_);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels_);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask_.data());
        for (std::size_t i = 0; i < capabilities_.size(); ++i)
            enabled_[i] = glIsEnabled(capabilities_[i]);
        for (std::size_t i = 0; i < textures_.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures_[i]);
            glGetIntegerv(GL_SAMPLER_BINDING, &samplers_[i]);
        }
        glActiveTexture(activeTexture_);
    }

    ~GlState() {
        glUseProgram(program_);
        glBindVertexArray(vao_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer_);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer_);
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        glColorMask(colorMask_[0], colorMask_[1], colorMask_[2], colorMask_[3]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength_);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows_);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels_);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer_);
        for (std::size_t i = 0; i < capabilities_.size(); ++i) {
            if (enabled_[i])
                glEnable(capabilities_[i]);
            else
                glDisable(capabilities_[i]);
        }
        for (std::size_t i = 0; i < textures_.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures_[i]);
            glBindSampler(i, samplers_[i]);
        }
        glActiveTexture(activeTexture_);
    }

  private:
    GLint program_ = 0, vao_ = 0, drawFramebuffer_ = 0, readFramebuffer_ = 0, activeTexture_ = 0;
    GLint unpackAlignment_ = 4, unpackBuffer_ = 0, unpackRowLength_ = 0, unpackSkipRows_ = 0, unpackSkipPixels_ = 0;
    std::array<GLint, 4> viewport_{}, scissor_{};
    std::array<GLboolean, 4> colorMask_{};
    std::array<GLint, 3> textures_{}, samplers_{};
    static constexpr std::array<GLenum, 8> capabilities_{
        GL_BLEND, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD,
        GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE};
    std::array<GLboolean, capabilities_.size()> enabled_{};
};

constexpr const char* vertexSource = R"GLSL(#version 300 es
precision highp float;
out vec2 uv;
void main() {
    vec2 position = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Fade, wipe and radial reveal draw on Noctalia v5.1.0's two-source GLSL design.
// Attribution and MIT terms: ../third_party/noctalia-wallpaper.LICENSE.
constexpr const char* fragmentSource = R"GLSL(#version 300 es
precision highp float;
precision highp int;
in vec2 uv;
out vec4 fragment;
uniform highp sampler2D source1;
uniform highp sampler2D source2;
uniform highp sampler2D masks;
uniform vec2 outputSize;
uniform vec2 cursor;
uniform vec2 origin;
uniform vec4 image1;
uniform vec4 image2;
uniform vec2 position1;
uniform vec2 position2;
uniform vec3 fallbackColor;
uniform float progress;
uniform float antialias;
uniform int transitionType;
uniform int maskCount;
uniform bool revealEnabled;

vec3 wallpaper(highp sampler2D tex, vec4 image, vec2 focal) {
    if (image.w < 0.5) return fallbackColor;
    vec2 imageUv = uv;
    if (image.z < 1.5) {
        vec2 ratios = outputSize / image.xy;
        float scale = image.z < 0.5 ? max(ratios.x, ratios.y) : min(ratios.x, ratios.y);
        vec2 scaled = image.xy * scale;
        imageUv = (uv * outputSize - (outputSize - scaled) * focal) / scaled;
    }
    if (any(lessThan(imageUv, vec2(0.0))) || any(greaterThan(imageUv, vec2(1.0)))) return fallbackColor;
    vec4 color = texture(tex, imageUv);
    return color.rgb + fallbackColor * (1.0 - color.a);
}

float transitionFactor(vec2 point) {
    if (transitionType == 0 || progress >= 1.0) return 1.0;
    if (progress <= 0.0) return 0.0;
    if (transitionType == 2) {
        bool fromRight = origin.x > outputSize.x * 0.5;
        float edge = fromRight ? outputSize.x * (1.0 - progress) : outputSize.x * progress;
        float factor = smoothstep(edge - antialias, edge + antialias, point.x);
        return fromRight ? factor : 1.0 - factor;
    }
    if (transitionType == 3) {
        float maxRadius = max(max(length(origin), length(outputSize - origin)),
                              max(length(origin - vec2(outputSize.x, 0.0)), length(origin - vec2(0.0, outputSize.y))));
        float radius = maxRadius * progress;
        return 1.0 - smoothstep(radius - antialias, radius + antialias, length(point - origin));
    }
    if (transitionType == 4) {
        vec2 low = origin * progress;
        vec2 high = origin + (outputSize - origin) * (1.0 - progress);
        float inside = min(min(point.x - low.x, high.x - point.x), min(point.y - low.y, high.y - point.y));
        return 1.0 - smoothstep(-antialias, antialias, inside);
    }
    if (transitionType == 5) {
        vec2 delta = point - origin;
        if (dot(delta, delta) <= 0.00000001) return 1.0;
        float angle = atan(delta.y, delta.x) + 1.57079632679;
        float turn = mod(angle + 6.28318530718, 6.28318530718) / 6.28318530718;
        float soft = antialias / max(length(delta), antialias) / 6.28318530718;
        return 1.0 - smoothstep(progress - soft, progress + soft, turn);
    }
    return progress;
}

float edgeReveal(float distance, float softness) {
    if (softness <= 0.0001) return distance <= 0.0 ? 1.0 : 0.0;
    return clamp(-distance / softness, 0.0, 1.0);
}
float segmentDistance(vec2 point, vec2 start, vec2 end) {
    vec2 segment = end - start;
    float squared = dot(segment, segment);
    if (squared <= 0.00000001) return length(point - start);
    return length(point - start - segment * clamp(dot(point - start, segment) / squared, 0.0, 1.0));
}
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
float triangleDistance(vec2 point, vec2 a, vec2 b, vec2 c) {
    vec3 sides = vec3(cross2(b - a, point - a), cross2(c - b, point - b), cross2(a - c, point - c));
    bool inside = !(any(lessThan(sides, vec3(0.0))) && any(greaterThan(sides, vec3(0.0))));
    float distance = min(min(segmentDistance(point, a, b), segmentDistance(point, b, c)), segmentDistance(point, c, a));
    return inside ? -distance : distance;
}
vec4 evaluatedMask(int index, vec2 point) {
    vec4 settings = texelFetch(masks, ivec2(0, index), 0);
    vec4 colorRadius = texelFetch(masks, ivec2(1, index), 0);
    vec4 geometry = texelFetch(masks, ivec2(2, index), 0);
    vec4 fan = texelFetch(masks, ivec2(3, index), 0);
    float reveal = 0.0;
    if (revealEnabled) {
        if (settings.x < 1.5) {
            reveal = edgeReveal(length(point - cursor) - colorRadius.w, geometry.x);
        } else if (settings.x < 2.5) {
            float distance = settings.w < 0.5 ? abs(point.y - cursor.y) : abs(point.x - cursor.x);
            reveal = edgeReveal(distance - geometry.y * 0.5, geometry.x);
        } else {
            vec2 anchor = geometry.zw * outputSize;
            vec2 direction = fan.zw;
            vec2 transverse = vec2(-direction.y, direction.x);
            vec2 relative = point - cursor;
            float radius = max(colorRadius.w, 0.0001);
            float longitudinal = max(radius * fan.x, 0.0001);
            vec2 ellipse = vec2(dot(relative, direction) / longitudinal, dot(relative, transverse) / radius);
            float ellipseReveal = edgeReveal((length(ellipse) - 1.0) * min(radius, longitudinal), geometry.x);
            float anchorDistance = length(cursor - anchor);
            reveal = ellipseReveal;
            if (anchorDistance > 0.0001) {
                float triangleReveal = edgeReveal(triangleDistance(point, anchor, cursor + transverse * radius, cursor - transverse * radius), geometry.x);
                float beamProgress = clamp(dot(point - anchor, direction) / anchorDistance, 0.0, 1.0);
                reveal = max(reveal, triangleReveal * mix(fan.y, 1.0, beamProgress));
            }
        }
    }
    float alpha = settings.y * settings.z * (1.0 - reveal);
    return vec4(colorRadius.rgb * alpha, alpha);
}
void main() {
    vec2 point = uv * outputSize;
    float factor = transitionFactor(point);
    vec3 color = wallpaper(source2, image2, position2);
    if (transitionType != 0 && progress < 1.0)
        color = mix(wallpaper(source1, image1, position1), color, factor);
    vec4 mask = vec4(0.0);
    for (int index = 0; index < maskCount; ++index) mask += evaluatedMask(index, point);
    fragment = vec4(color * (1.0 - mask.a) + mask.rgb, 1.0);
}
)GLSL";

GLuint compile(const GLenum type, const char* source) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string error(std::max(length, 1), '\0');
        glGetShaderInfoLog(shader, length, nullptr, error.data());
        glDeleteShader(shader);
        throw std::runtime_error("Luxaxis shader compile: " + error);
    }
    return shader;
}

struct Mask {
    Spotlight spotlight;
    double weight = 1.0;
    Vec2 direction{0.0, 1.0};
    friend bool operator==(const Mask&, const Mask&) = default;
};

struct Frame {
    Image from, to;
    std::vector<Mask> masks;
    Color fallback;
    Vec2 logicalSize, pixelSize, cursor, origin;
    TransitionType transition = TransitionType::None;
    double progress = 1.0;
    bool reveal = false;
    bool spotlightEnabled = true;
    friend bool operator==(const Frame&, const Frame&) = default;
};

std::optional<Rect> effectDamage(const Frame& frame, const Vec2 cursor) {
    if (!frame.spotlightEnabled)
        return std::nullopt;
    Vec2 low = frame.logicalSize, high{};
    bool hasBounds = false;
    for (const auto point : {frame.cursor, cursor}) {
        for (const auto& mask : frame.masks) {
            if (const auto bounds = spotlightEffectBounds({mask.spotlight, frame.logicalSize, point, frame.reveal, mask.direction})) {
                low = {std::min(low.x, bounds->position.x), std::min(low.y, bounds->position.y)};
                high = {std::max(high.x, bounds->position.x + bounds->size.x), std::max(high.y, bounds->position.y + bounds->size.y)};
                hasBounds = true;
            }
        }
    }
    if (!hasBounds)
        return std::nullopt;
    // Cover a physical-pixel fringe at fractional scales, not just one logical pixel.
    const auto fringe = std::max({1.0, frame.logicalSize.x / frame.pixelSize.x, frame.logicalSize.y / frame.pixelSize.y});
    low = {std::max(0.0, low.x - fringe), std::max(0.0, low.y - fringe)};
    high = {std::min(frame.logicalSize.x, high.x + fringe), std::min(frame.logicalSize.y, high.y + fringe)};
    return Rect{low, {high.x - low.x, high.y - low.y}};
}

Spotlight interpolate(Spotlight from, const Spotlight& to, const double p, const Vec2 size) {
    const auto shortEdge = std::min(size.x, size.y);
    const auto length = [shortEdge, p](const Length& a, const Length& b) {
        return Length{std::lerp(a.resolve(shortEdge), b.resolve(shortEdge), p), LengthUnit::Pixels};
    };
    from.maskColor = {std::lerp(from.maskColor.r, to.maskColor.r, p), std::lerp(from.maskColor.g, to.maskColor.g, p), std::lerp(from.maskColor.b, to.maskColor.b, p)};
    from.maskOpacity = std::lerp(from.maskOpacity, to.maskOpacity, p);
    from.radius = length(from.radius, to.radius);
    from.softness = length(from.softness, to.softness);
    from.thickness = length(from.thickness, to.thickness);
    from.anchor = {std::lerp(from.anchor.x, to.anchor.x, p), std::lerp(from.anchor.y, to.anchor.y, p)};
    from.aspectRatio = std::lerp(from.aspectRatio, to.aspectRatio, p);
    from.beamStartReveal = std::lerp(from.beamStartReveal, to.beamStartReveal, p);
    if (p >= 0.5)
        from.orientation = to.orientation;
    return from;
}

std::vector<Mask> blendMasks(const std::vector<Mask>& from, const Spotlight& to, const double p, const Vec2 size) {
    if (from.size() == 1 && from.front().weight == 1.0 && from.front().spotlight.type == to.type && to.type != SpotlightType::None)
        return {{interpolate(from.front().spotlight, to, p, size), 1.0, from.front().direction}};
    std::vector<Mask> result;
    const auto add = [&result](Mask mask) {
        if (mask.spotlight.type == SpotlightType::None || mask.weight <= 0.0)
            return;
        const auto existing = std::ranges::find_if(result, [&mask](const auto& candidate) { return candidate.spotlight == mask.spotlight; });
        if (existing == result.end())
            result.push_back(std::move(mask));
        else
            existing->weight += mask.weight;
    };
    for (auto mask : from) {
        mask.weight *= 1.0 - p;
        add(std::move(mask));
    }
    add({to, p});
    return result;
}

} // namespace

Texture::Texture(const Vec2 size) : size_(size) {
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1 || size.y < 1 || size.x > limit || size.y > limit)
        throw std::runtime_error("Luxaxis GPU target exceeds the texture size limit");
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer_);
        glDeleteTextures(1, &texture_);
        framebuffer_ = texture_ = 0;
        throw std::runtime_error("Luxaxis GPU framebuffer is incomplete");
    }
}

Texture::~Texture() {
    glDeleteFramebuffers(1, &framebuffer_);
    glDeleteTextures(1, &texture_);
}

struct RendererImpl {
    GLuint program = 0, vao = 0, parameters = 0;
    struct ImageUniforms {
        GLint sampler, dimensions, position;
    };
    struct Uniforms {
        GLint outputSize, cursor, origin, fallbackColor, progress, antialias, transitionType, revealEnabled, masks, maskCount;
        ImageUniforms from, to;
    } uniforms{};
    std::vector<float> lastParameters;
    std::shared_ptr<Texture> output;
    std::array<std::shared_ptr<Texture>, 2> snapshots;
    std::optional<Frame> lastFrame;
    Image source;
    std::vector<Mask> sourceMasks;
    std::uint64_t transitionId = 0;
    Statistics stats;

    ~RendererImpl() {
        if (program) glDeleteProgram(program);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (parameters) glDeleteTextures(1, &parameters);
    }

    void initialize() {
        if (program)
            return;
        GLuint vertex = 0, fragment = 0, candidate = 0;
        try {
            vertex = compile(GL_VERTEX_SHADER, vertexSource);
            fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
            candidate = glCreateProgram();
            glAttachShader(candidate, vertex);
            glAttachShader(candidate, fragment);
            glLinkProgram(candidate);
            GLint success = 0;
            glGetProgramiv(candidate, GL_LINK_STATUS, &success);
            if (!success) {
                GLint length = 0;
                glGetProgramiv(candidate, GL_INFO_LOG_LENGTH, &length);
                std::string error(std::max(length, 1), '\0');
                glGetProgramInfoLog(candidate, length, nullptr, error.data());
                throw std::runtime_error("Luxaxis shader link: " + error);
            }
        } catch (...) {
            if (candidate) glDeleteProgram(candidate);
            if (vertex) glDeleteShader(vertex);
            if (fragment) glDeleteShader(fragment);
            throw;
        }
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        program = candidate;
        const auto location = [this](const char* name) { return glGetUniformLocation(program, name); };
        uniforms = {location("outputSize"), location("cursor"), location("origin"), location("fallbackColor"),
                    location("progress"), location("antialias"), location("transitionType"), location("revealEnabled"),
                    location("masks"), location("maskCount"),
                    {location("source1"), location("image1"), location("position1")},
                    {location("source2"), location("image2"), location("position2")}};
        glGenVertexArrays(1, &vao);
        glGenTextures(1, &parameters);
    }

    void draw(const Frame& frame, const std::shared_ptr<Texture>& target, const bool wallpaperOnly = false, const std::optional<Rect>& dirty = {}) {
        initialize();
        glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer_);
        glViewport(0, 0, target->size_.x, target->size_.y);
        for (const auto cap : {GL_BLEND, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD,
                               GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE})
            glDisable(cap);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        auto pixelsDrawn = static_cast<std::uint64_t>(target->size_.x * target->size_.y);
        if (dirty) {
            const auto x = std::floor(dirty->position.x * frame.pixelSize.x / frame.logicalSize.x);
            const auto y = std::floor(dirty->position.y * frame.pixelSize.y / frame.logicalSize.y);
            const auto right = std::ceil((dirty->position.x + dirty->size.x) * frame.pixelSize.x / frame.logicalSize.x);
            const auto bottom = std::ceil((dirty->position.y + dirty->size.y) * frame.pixelSize.y / frame.logicalSize.y);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, right - x, bottom - y);
            pixelsDrawn = static_cast<std::uint64_t>((right - x) * (bottom - y));
        }
        glUseProgram(program);
        glBindVertexArray(vao);
        glUniform2f(uniforms.outputSize, frame.logicalSize.x, frame.logicalSize.y);
        glUniform2f(uniforms.cursor, frame.cursor.x, frame.cursor.y);
        glUniform2f(uniforms.origin, frame.origin.x, frame.origin.y);
        glUniform3f(uniforms.fallbackColor, frame.fallback.r, frame.fallback.g, frame.fallback.b);
        glUniform1f(uniforms.progress, frame.progress);
        glUniform1f(uniforms.antialias, 0.5 * std::max(frame.logicalSize.x / frame.pixelSize.x, frame.logicalSize.y / frame.pixelSize.y));
        glUniform1i(uniforms.transitionType, static_cast<int>(frame.transition));
        glUniform1i(uniforms.revealEnabled, frame.reveal);
        const auto bindImage = [](const Image& image, const GLuint unit, const ImageUniforms& uniform) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindSampler(unit, 0);
            glBindTexture(GL_TEXTURE_2D, image.texture);
            glUniform1i(uniform.sampler, unit);
            glUniform4f(uniform.dimensions, image.size.x, image.size.y, static_cast<int>(image.fit), image.texture != 0);
            glUniform2f(uniform.position, image.position.x, image.position.y);
        };
        bindImage(frame.from, 0, uniforms.from);
        bindImage(frame.to, 1, uniforms.to);
        std::vector<float> packed;
        const auto shortEdge = std::min(frame.logicalSize.x, frame.logicalSize.y);
        if (!wallpaperOnly && frame.spotlightEnabled) {
            packed.reserve(frame.masks.size() * 16);
            for (const auto& mask : frame.masks) {
                const auto& s = mask.spotlight;
                for (const auto value : std::array<double, 16>{
                         static_cast<double>(s.type), s.maskOpacity, mask.weight, static_cast<double>(s.orientation),
                         s.maskColor.r, s.maskColor.g, s.maskColor.b, s.radius.resolve(shortEdge),
                         s.softness.resolve(shortEdge), s.thickness.resolve(shortEdge), s.anchor.x, s.anchor.y,
                         s.aspectRatio, s.beamStartReveal, mask.direction.x, mask.direction.y})
                    packed.push_back(static_cast<float>(value));
            }
        }
        if (packed.empty())
            packed.resize(16, 0.0F);
        glActiveTexture(GL_TEXTURE2);
        glBindSampler(2, 0);
        glBindTexture(GL_TEXTURE_2D, parameters);
        if (packed != lastParameters) {
            GLint limit = 0;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
            if (packed.size() / 16 > static_cast<std::size_t>(limit))
                throw std::runtime_error("Luxaxis interrupted Spotlight state exceeds the GPU parameter limit");
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            if (packed.size() != lastParameters.size()) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, packed.size() / 16, 0, GL_RGBA, GL_FLOAT, packed.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, packed.size() / 16, GL_RGBA, GL_FLOAT, packed.data());
            }
            lastParameters = std::move(packed);
        }
        glUniform1i(uniforms.masks, 2);
        glUniform1i(uniforms.maskCount, wallpaperOnly || !frame.spotlightEnabled ? 0 : frame.masks.size());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (const auto error = glGetError(); error != GL_NO_ERROR)
            throw std::runtime_error("Luxaxis GPU draw failed with GL error " + std::to_string(error));
        ++stats.draws;
        stats.pixelsDrawn += pixelsDrawn;
    }

    void capture() {
        const auto slot = snapshots[0] && lastFrame->from.texture == snapshots[0]->id() ? 1 : 0;
        auto& target = snapshots[slot];
        if (!target || target->size() != lastFrame->pixelSize)
            target = std::shared_ptr<Texture>(new Texture(lastFrame->pixelSize));
        draw(*lastFrame, target, true);
        source = {target->id(), target->size(), FitMode::Stretch, {0.5, 0.5}, target};
        ++stats.snapshots;
    }

    std::shared_ptr<const Texture> render(const Input& input) {
        if (!std::isfinite(input.logicalSize.x) || !std::isfinite(input.logicalSize.y) || input.logicalSize.x <= 0 || input.logicalSize.y <= 0)
            throw std::runtime_error("Luxaxis GPU output has invalid logical dimensions");
        GlState restore;
        const auto resized = !output || output->size() != input.pixelSize;
        if (resized)
            output = std::shared_ptr<Texture>(new Texture(input.pixelSize));

        Frame frame{
            .from = input.destination,
            .to = input.destination,
            .masks = {},
            .fallback = input.fallbackColor,
            .logicalSize = input.logicalSize,
            .pixelSize = input.pixelSize,
            .cursor = input.cursor,
            .origin = input.transitionOrigin,
            .transition = input.transitioning ? input.transition.type : TransitionType::None,
            .progress = easeProgress(input.progress, input.transition.easing),
            .reveal = input.revealEnabled,
            .spotlightEnabled = input.spotlightEnabled,
        };
        if (!input.destinationReady && lastFrame) {
            frame = *lastFrame;
            frame.origin = {frame.origin.x * input.logicalSize.x / frame.logicalSize.x,
                            frame.origin.y * input.logicalSize.y / frame.logicalSize.y};
            frame.logicalSize = input.logicalSize;
            frame.pixelSize = input.pixelSize;
            frame.cursor = input.cursor;
            frame.reveal = input.revealEnabled;
            frame.spotlightEnabled = input.spotlightEnabled;
        } else if (input.transitioning && lastFrame) {
            if (input.transitionId != transitionId) {
                sourceMasks = lastFrame->masks;
                if (lastFrame->fallback == input.fallbackColor &&
                    (lastFrame->transition == TransitionType::None || lastFrame->progress >= 1.0)) {
                    source = lastFrame->to;
                } else {
                    capture();
                }
                transitionId = input.transitionId;
            }
            frame.from = source;
            if (source == input.destination)
                frame.transition = TransitionType::None;
            frame.masks = blendMasks(sourceMasks, input.spotlight, frame.progress, input.logicalSize);
        } else {
            if (input.spotlight.type != SpotlightType::None)
                frame.masks = {{input.spotlight}};
            transitionId = input.transitionId;
        }

        for (auto& mask : frame.masks) {
            const auto anchor = Vec2{mask.spotlight.anchor.x * frame.logicalSize.x, mask.spotlight.anchor.y * frame.logicalSize.y};
            auto previous = mask.direction;
            if (lastFrame) {
                const auto old = std::ranges::find_if(lastFrame->masks, [&mask](const auto& candidate) { return candidate.spotlight == mask.spotlight; });
                if (old != lastFrame->masks.end())
                    previous = old->direction;
            }
            mask.direction = resolveFanDirection(anchor, frame.cursor, previous);
        }

        // Cursor-origin metadata must not invalidate draws that never use it.
        if (frame.transition == TransitionType::None || frame.transition == TransitionType::Fade)
            frame.origin = {};
        if (frame.masks.empty() || !frame.spotlightEnabled)
            frame.reveal = false;

        const auto releaseCompleted = [this, &input] {
            if (!input.transitioning && input.destinationReady) {
                snapshots = {};
                source = {};
                sourceMasks.clear();
            }
        };
        if (lastFrame && *lastFrame == frame) {
            releaseCompleted();
            return output;
        }
        std::optional<Rect> dirty;
        if (lastFrame) {
            auto previous = *lastFrame;
            previous.cursor = frame.cursor;
            for (auto& mask : previous.masks) {
                const auto found = std::ranges::find_if(frame.masks, [&mask](const auto& candidate) { return candidate.spotlight == mask.spotlight; });
                if (found != frame.masks.end())
                    mask.direction = found->direction;
            }
            if (previous == frame) {
                dirty = effectDamage(*lastFrame, frame.cursor);
                if (!dirty) {
                    lastFrame = std::move(frame);
                    releaseCompleted();
                    return output;
                }
            }
        }
        draw(frame, output, false, dirty);
        lastFrame = std::move(frame);
        releaseCompleted();
        return output;
    }
};

Renderer::Renderer() : impl_(std::make_unique<RendererImpl>()) {}
Renderer::~Renderer() = default;

std::shared_ptr<const Texture> Renderer::render(const Input& input) { return impl_->render(input); }

std::optional<Rect> Renderer::cursorDamage(const Vec2 cursor) const {
    return impl_->lastFrame ? effectDamage(*impl_->lastFrame, cursor) : std::nullopt;
}

Statistics Renderer::statistics() const {
    auto result = impl_->stats;
    const auto bytes = [](const auto& texture) -> std::size_t {
        return texture ? static_cast<std::size_t>(texture->size().x) * static_cast<std::size_t>(texture->size().y) * 4U : 0;
    };
    result.residentBytes = bytes(impl_->output) + bytes(impl_->snapshots[0]) + bytes(impl_->snapshots[1]) + impl_->lastParameters.size() * sizeof(float);
    return result;
}

} // namespace luxaxis::gpu
