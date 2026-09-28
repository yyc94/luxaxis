#include "luxaxis/gpu_renderer.hpp"
#include "luxaxis/engine.hpp"
#include "luxaxis/spotlight.hpp"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace luxaxis;

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct EglContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    EglContext() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "cannot initialize surfaceless EGL");
        require(eglBindAPI(EGL_OPENGL_ES_API), "cannot bind GLES");
        const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config{};
        EGLint count = 0;
        require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "no GLES 3 EGL config");
        const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        const EGLint surfaceAttributes[]{EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
        require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context), "cannot make GLES context current");
        std::cout << "GLES renderer: " << glGetString(GL_RENDERER) << "\nGLES version: " << glGetString(GL_VERSION) << '\n';
    }

    ~EglContext() {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
    }
};

struct ImageFixture {
    GLuint id = 0;
    Vec2 size;

    explicit ImageFixture(const std::array<std::uint8_t, 4> color) : ImageFixture({1, 1}, std::vector<std::uint8_t>{color.begin(), color.end()}) {}

    ImageFixture(Vec2 dimensions, const std::vector<std::uint8_t>& pixels) : size(dimensions) {
        glGenTextures(1, &id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    ~ImageFixture() { glDeleteTextures(1, &id); }
    gpu::Image image() const { return {id, size, FitMode::Stretch, {0.5, 0.5}, {}}; }
};

std::vector<std::uint8_t> pixels(const std::shared_ptr<const gpu::Texture>& texture) {
    GLint oldDraw = 0, oldRead = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->id(), 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "test output is not a complete framebuffer");
    const auto size = texture->size();
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size.x * size.y) * 4);
    glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDraw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, oldRead);
    glDeleteFramebuffers(1, &framebuffer);
    require(glGetError() == GL_NO_ERROR, "GLES error during actual shader rendering/readback");
    return result;
}

void writePpm(const std::string& path, const std::vector<std::uint8_t>& data, const int width, const int height) {
    std::ofstream output(path, std::ios::binary);
    require(output.good(), "cannot open GPU preview output " + path);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y * width + x) * 4;
            const std::array<char, 3> rgb{static_cast<char>(data[offset]), static_cast<char>(data[offset + 1]), static_cast<char>(data[offset + 2])};
            output.write(rgb.data(), rgb.size());
        }
    }
    require(output.good(), "failed writing GPU preview output " + path);
}

void colorAt(const std::vector<std::uint8_t>& data, const int width, const int x, const int y, const std::array<int, 3> expected, const std::string& message, const int tolerance = 2) {
    const auto offset = static_cast<std::size_t>(y * width + x) * 4;
    for (int channel = 0; channel < 3; ++channel) {
        require(std::abs(static_cast<int>(data[offset + channel]) - expected[channel]) <= tolerance,
                message + " channel " + std::to_string(channel) + ": got " + std::to_string(data[offset + channel]) + ", expected " + std::to_string(expected[channel]));
    }
    require(data[offset + 3] == 255, message + " output must be opaque");
}

gpu::Input input(const ImageFixture& image) {
    gpu::Input result;
    result.destination = image.image();
    result.logicalSize = {1000, 800};
    result.pixelSize = {100, 80};
    result.cursor = {505, 405};
    result.transitionOrigin = {500, 400};
    return result;
}

Transition transition(const TransitionType type = TransitionType::Fade) {
    Transition result;
    result.type = type;
    result.easing = "linear";
    return result;
}

Spotlight spotlight(const SpotlightType type) {
    return {.type = type, .maskColor = {0, 0, 0}, .maskOpacity = 0.6,
            .radius = {160, LengthUnit::Pixels}, .softness = {20, LengthUnit::Pixels},
            .orientation = StripOrientation::Horizontal, .thickness = {200, LengthUnit::Pixels},
            .anchor = {0.5, 0.0}, .aspectRatio = 0.5, .beamStartReveal = 0.25};
}

void spotlightPixelsAndReferenceGeometry() {
    const ImageFixture white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    frame.revealEnabled = true;
    frame.spotlight = spotlight(SpotlightType::Circle);
    auto rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 50, 40, {255, 255, 255}, "circle center");
    colorAt(rendered, 100, 65, 40, {179, 179, 179}, "circle soft boundary");
    colorAt(rendered, 100, 90, 40, {102, 102, 102}, "circle exterior");

    for (const auto type : {SpotlightType::None, SpotlightType::Circle, SpotlightType::Strip, SpotlightType::Fan}) {
        for (const auto orientation : {StripOrientation::Horizontal, StripOrientation::Vertical}) {
            frame.spotlight = spotlight(type);
            frame.spotlight.orientation = orientation;
            if (orientation == StripOrientation::Vertical)
                frame.spotlight.anchor = {0, 0.5};
            rendered = pixels(renderer.render(frame));
            const SpotlightSample sample{frame.spotlight, frame.logicalSize, frame.cursor, true};
            for (int y = 0; y < 80; y += 3) {
                for (int x = 0; x < 100; x += 3) {
                    const auto alpha = maskAlphaAt(sample, {(x + 0.5) * 10, (y + 0.5) * 10});
                    const auto expected = static_cast<int>(std::lround((1.0 - alpha) * 255));
                    colorAt(rendered, 100, x, y, {expected, expected, expected}, "GPU Spotlight matches reference");
                }
            }
        }
        frame.revealEnabled = false;
        rendered = pixels(renderer.render(frame));
        const auto expected = type == SpotlightType::None ? 255 : 102;
        colorAt(rendered, 100, 50, 40, {expected, expected, expected}, "inactive output");
        frame.revealEnabled = true;
    }
    frame.spotlight = spotlight(SpotlightType::Fan);
    frame.spotlight.anchor = {0.505, 0.50625};
    rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 50, 40, {255, 255, 255}, "degenerate fan retains ellipse");
}

void fitFocalPointAlphaAndImageOrientation() {
    const ImageFixture quadrants{{2, 2}, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(quadrants);
    auto rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 10, 10, {255, 0, 0}, "top left orientation");
    colorAt(rendered, 100, 90, 10, {0, 255, 0}, "top right orientation");
    colorAt(rendered, 100, 10, 70, {0, 0, 255}, "bottom left orientation");
    frame.destination.fit = FitMode::Contain;
    rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 1, 40, {0, 0, 0}, "contain letterbox");

    const ImageFixture wide{{4, 1}, {255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255}};
    frame.destination = wide.image();
    frame.destination.fit = FitMode::Cover;
    frame.destination.position = {0, 0.5};
    rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 50, 40, {255, 0, 0}, "cover left focal point");
    frame.destination.position.x = 1;
    rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 50, 40, {0, 0, 255}, "cover right focal point");

    const ImageFixture transparent{{128, 0, 0, 128}};
    frame.destination = transparent.image();
    frame.fallbackColor = {0, 0, 1};
    rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 50, 40, {128, 0, 127}, "premultiplied wallpaper over fallback");

    frame.transition = transition();
    frame.transitioning = true;
    frame.transitionId = 1;
    frame.progress = 0;
    frame.fallbackColor = {0, 1, 0};
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {128, 0, 127}, "fallback change preserves the visible source color");
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {128, 64, 64}, "fallback color participates in the transition");
}

void transitionsUseShadersAndEasing() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}};
    for (const auto type : {TransitionType::Fade, TransitionType::Wipe, TransitionType::Grow, TransitionType::Outer, TransitionType::Clock}) {
        gpu::Renderer renderer;
        auto frame = input(red);
        (void)renderer.render(frame);
        frame.destination = blue.image();
        frame.transition = transition(type);
        frame.transitioning = true;
        frame.transitionId = 1;
        frame.progress = 0;
        auto rendered = pixels(renderer.render(frame));
        colorAt(rendered, 100, 20, 20, {255, 0, 0}, "transition starts from visible source");
        frame.progress = 0.5;
        rendered = pixels(renderer.render(frame));
        if (type == TransitionType::Fade) {
            colorAt(rendered, 100, 20, 20, {128, 0, 128}, "GPU fade midpoint");
            frame.transition.easing = "ease-in";
            rendered = pixels(renderer.render(frame));
            colorAt(rendered, 100, 20, 20, {223, 0, 32}, "GPU fade uses configured easing");
        } else if (type == TransitionType::Wipe) {
            colorAt(rendered, 100, 20, 20, {0, 0, 255}, "wipe revealed side");
            colorAt(rendered, 100, 80, 20, {255, 0, 0}, "wipe hidden side");
        } else if (type == TransitionType::Grow) {
            colorAt(rendered, 100, 50, 40, {0, 0, 255}, "grow origin");
            colorAt(rendered, 100, 1, 1, {255, 0, 0}, "grow distant corner");
        } else if (type == TransitionType::Outer) {
            colorAt(rendered, 100, 1, 1, {0, 0, 255}, "outer edge");
            colorAt(rendered, 100, 50, 40, {255, 0, 0}, "outer center");
        } else {
            colorAt(rendered, 100, 80, 40, {0, 0, 255}, "clock revealed side");
            colorAt(rendered, 100, 20, 40, {255, 0, 0}, "clock hidden side");
        }
        frame.progress = 1;
        rendered = pixels(renderer.render(frame));
        colorAt(rendered, 100, 1, 1, {0, 0, 255}, "transition completes fully");
    }
}

void clockOriginIsDefinedForEveryDriver() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(red);
    (void)renderer.render(frame);
    frame.destination = blue.image();
    frame.transition = transition(TransitionType::Clock);
    frame.transitionOrigin = frame.cursor;
    frame.transitioning = true;
    frame.transitionId = 1;
    frame.progress = 0;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {255, 0, 0}, "clock origin starts with the old wallpaper");
    frame.progress = 0.001;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {0, 0, 255}, "every nonempty angular sector contains its origin");
}

void repeatedInterruptionsCaptureWithoutQueueing() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}}, green{{0, 255, 0, 255}}, white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(red);
    (void)renderer.render(frame);
    frame.transitioning = true;
    frame.transition = transition();
    frame.transitionId = 1;
    frame.destination = blue.image();
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 100, 10, 10, {128, 0, 128}, "A to B midpoint");
    frame.destination = green.image();
    frame.transitionId = 2;
    frame.progress = 0;
    colorAt(pixels(renderer.render(frame)), 100, 10, 10, {128, 0, 128}, "first interruption is continuous");
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 100, 10, 10, {64, 128, 64}, "new target starts immediately");
    frame.destination = white.image();
    frame.transitionId = 3;
    frame.progress = 0;
    colorAt(pixels(renderer.render(frame)), 100, 10, 10, {64, 128, 64}, "second interruption retains A contribution");
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 100, 10, 10, {160, 192, 160}, "repeated interrupt GPU snapshot");
    require(renderer.statistics().snapshots == 2, "only mixed wallpaper interruptions need GPU snapshots");
    frame.transitioning = false;
    frame.progress = 1;
    colorAt(pixels(renderer.render(frame)), 100, 10, 10, {255, 255, 255}, "latest destination completes");
    require(renderer.statistics().residentBytes == 100U * 80U * 4U + 64U, "finished transition releases snapshot textures");
}

void geometricInterruptionsPreserveEveryPixel() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}}, green{{0, 255, 0, 255}}, white{{255, 255, 255, 255}};
    for (const auto type : {TransitionType::Wipe, TransitionType::Grow, TransitionType::Outer, TransitionType::Clock}) {
        gpu::Renderer renderer;
        auto frame = input(red);
        (void)renderer.render(frame);
        frame.transition = transition(type);
        frame.transitioning = true;
        frame.transitionId = 1;
        frame.destination = blue.image();
        frame.progress = 0.5;
        auto before = pixels(renderer.render(frame));
        require(renderer.statistics().snapshots == 0, "a normal transition should sample the old wallpaper directly");
        for (const auto* target : {&green, &white}) {
            frame.destination = target->image();
            ++frame.transitionId;
            frame.progress = 0;
            const auto after = pixels(renderer.render(frame));
            require(after == before, "geometric interruption changed the current wallpaper pixels");
            frame.progress = 0.25;
            before = pixels(renderer.render(frame));
            require(before != after, "geometric interruption did not start toward the newest destination");
        }
        frame.progress = 1;
        frame.transitioning = false;
        colorAt(pixels(renderer.render(frame)), 100, 10, 10, {255, 255, 255}, "geometric interruption completes at latest target");
    }
}

void cursorRemainsLiveAcrossMaskInterruptions() {
    const ImageFixture white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    frame.logicalSize = frame.pixelSize = {100, 100};
    frame.cursor = {20.5, 20.5};
    frame.revealEnabled = true;
    frame.spotlight = spotlight(SpotlightType::Circle);
    frame.spotlight.radius = {15, LengthUnit::Pixels};
    frame.spotlight.softness = {2, LengthUnit::Pixels};
    (void)renderer.render(frame);
    frame.transitioning = true;
    frame.transition = transition();
    frame.transitionId = 1;
    frame.progress = 0.5;
    frame.spotlight.type = SpotlightType::Strip;
    frame.spotlight.thickness = {15, LengthUnit::Pixels};
    (void)renderer.render(frame);
    frame.transitionId = 2;
    frame.progress = 0;
    frame.spotlight.type = SpotlightType::Fan;
    (void)renderer.render(frame);
    frame.cursor = {80.5, 80.5};
    const auto rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 80, 80, {255, 255, 255}, "interrupted Spotlight follows new cursor");
    colorAt(rendered, 100, 20, 20, {102, 102, 102}, "old cursor is no longer revealed");
    require(renderer.statistics().snapshots == 0, "same-wallpaper mask transition skips wallpaper snapshots");
}

void engineKeepsInterruptedRevealLiveWhenLatestProfilesUseNone() {
    const ImageFixture white{{255, 255, 255, 255}};
    Config config;
    config.defaultProfile = "circle";
    config.transition = transition();
    config.transition->durationMs = 100;
    Profile circle;
    circle.wallpaper = "/wall/white.png";
    circle.spotlight = spotlight(SpotlightType::Circle);
    circle.spotlight.radius = {15, LengthUnit::Pixels};
    circle.spotlight.softness = {2, LengthUnit::Pixels};
    config.profiles.emplace("circle", circle);
    circle.spotlight = {};
    config.profiles.emplace("plain", circle);
    config.workspaces = {{1, "circle"}, {2, "plain"}, {3, "plain"}};
    Engine engine{config};
    require(engine.upsertOutput({"test", {{0, 0}, {100, 100}}, 1}), "test output was not created");
    require(engine.setCursor({20.5, 20.5}), "test cursor did not move");
    gpu::Renderer renderer;
    const auto render = [&] {
        const auto plan = engine.planFor("test");
        auto frame = input(white);
        frame.logicalSize = frame.pixelSize = plan->logicalBounds.size;
        frame.spotlight = plan->profile.spotlight;
        frame.cursor = plan->cursorLocal;
        frame.revealEnabled = plan->revealEnabled;
        frame.transition = plan->transition;
        frame.progress = plan->transitionProgress;
        frame.transitionId = plan->transitionId;
        frame.transitioning = plan->transitioning;
        return pixels(renderer.render(frame));
    };
    (void)render();
    require(engine.activateWorkspace("test", 2), "first switch failed");
    engine.advance(std::chrono::milliseconds{50});
    (void)render();
    require(engine.activateWorkspace("test", 3), "second switch failed");
    require(engine.setCursor({80.5, 80.5}), "interrupted cursor did not move");
    require(engine.planFor("test")->revealEnabled, "Engine disabled a still-visible interrupted Spotlight reveal");
    const auto rendered = render();
    colorAt(rendered, 100, 80, 80, {255, 255, 255}, "none destinations must not freeze an interrupted circle reveal");
    colorAt(rendered, 100, 20, 20, {179, 179, 179}, "interrupted source mask must follow the live cursor");
}

void sameTypeParametersAndSpotlightOverride() {
    const ImageFixture white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    frame.logicalSize = frame.pixelSize = {100, 100};
    frame.cursor = {50.5, 50.5};
    frame.revealEnabled = true;
    frame.spotlight = spotlight(SpotlightType::Circle);
    frame.spotlight.radius = {10, LengthUnit::Pixels};
    frame.spotlight.softness = {2, LengthUnit::Pixels};
    (void)renderer.render(frame);
    frame.spotlight.radius.value = 30;
    frame.spotlight.maskColor = {1, 0, 0};
    frame.spotlight.maskOpacity = 0.8;
    frame.transition = transition();
    frame.transitioning = true;
    frame.transitionId = 1;
    frame.progress = 0.5;
    auto rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 65, 50, {255, 255, 255}, "same-type radius interpolates rather than blending two masks");
    colorAt(rendered, 100, 1, 1, {166, 77, 77}, "same-type mask color and opacity interpolate");
    frame.spotlightEnabled = false;
    colorAt(pixels(renderer.render(frame)), 100, 1, 1, {255, 255, 255}, "Spotlight off is immediate during a transition");
    frame.spotlightEnabled = true;
    colorAt(pixels(renderer.render(frame)), 100, 1, 1, {166, 77, 77}, "Spotlight on restores the live transition");
    require(renderer.statistics().snapshots == 0, "same-texture Spotlight changes must not allocate snapshots");
}

void fractionalScaleDamageAndIndependentOutputs() {
    const ImageFixture white{{255, 255, 255, 255}}, red{{255, 0, 0, 255}};
    gpu::Renderer first, second, reference;
    auto frame = input(white);
    frame.logicalSize = {100, 100};
    frame.pixelSize = {125, 125};
    frame.cursor = {20.4, 20.4};
    frame.spotlight = spotlight(SpotlightType::Circle);
    frame.spotlight.radius = {12, LengthUnit::Pixels};
    frame.spotlight.softness = {3, LengthUnit::Pixels};
    frame.revealEnabled = true;
    (void)first.render(frame);
    auto other = input(red);
    other.logicalSize = {80, 120};
    other.pixelSize = {100, 150};
    const auto otherOutput = second.render(other);
    const auto otherDraws = second.statistics().draws;
    frame.cursor = {80.4, 80.4};
    const auto damage = first.cursorDamage(frame.cursor);
    require(damage && damage->contains({20.4, 20.4}) && damage->contains(frame.cursor), "cursor damage must include old and new logical effect bounds");
    require(damage->size.x < 100 && damage->size.y < 100, "circle cursor motion unnecessarily damages the full output");
    const auto rendered = pixels(first.render(frame));
    require(rendered == pixels(reference.render(frame)), "partial shader update differs from a complete fractional-scale render");
    colorAt(rendered, 125, 100, 100, {255, 255, 255}, "fractional-scale cursor center");
    colorAt(rendered, 125, 25, 25, {102, 102, 102}, "fractional-scale old cursor is cleared");
    colorAt(pixels(otherOutput), 100, 50, 75, {255, 0, 0}, "second portrait output is unaffected by first output cursor");
    require(second.statistics().draws == otherDraws, "one output caused another output to render");
    frame.revealEnabled = false;
    (void)first.render(frame);
    const auto draws = first.statistics().draws;
    frame.cursor = {10, 10};
    (void)first.render(frame);
    require(first.statistics().draws == draws && !first.cursorDamage({30, 30}), "inactive output cursor must not trigger shader work or damage");
}

void pendingWallpaperRetainsVisibleSourceAndIdleStopsDrawing() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(red);
    (void)renderer.render(frame);
    const auto draws = renderer.statistics().draws;
    for (int i = 0; i < 100; ++i)
        (void)renderer.render(frame);
    require(renderer.statistics().draws == draws, "unchanged wallpaper must not redraw its GPU target");
    frame.cursor = {100, 200};
    frame.transitionOrigin = frame.cursor;
    (void)renderer.render(frame);
    require(renderer.statistics().draws == draws, "cursor motion without Spotlight must not redraw its GPU target");
    frame.destination = blue.image();
    frame.destinationReady = false;
    frame.transitioning = true;
    frame.transitionId = 1;
    frame.transition = transition();
    frame.progress = 0;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {255, 0, 0}, "pending decode retains old wallpaper");
    frame.destinationReady = true;
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {128, 0, 128}, "ready destination transitions from retained wallpaper");

    const ImageFixture green{{0, 255, 0, 255}}, white{{255, 255, 255, 255}};
    frame.destination = green.image();
    frame.transitionId = 2;
    frame.progress = 0;
    frame.destinationReady = false;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {128, 0, 128}, "pending interruption retains the actual mixed source");
    frame.destination = white.image();
    frame.transitionId = 3;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {128, 0, 128}, "newer pending destination replaces older pending destination without queueing");
    frame.destinationReady = true;
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 100, 50, 40, {192, 128, 192}, "newest decoded destination transitions immediately from mixed source");
}

void pendingWallpaperKeepsCursorAndTemporaryOverridesLive() {
    const ImageFixture white{{255, 255, 255, 255}}, red{{255, 0, 0, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    frame.logicalSize = frame.pixelSize = {100, 100};
    frame.cursor = {20.5, 20.5};
    frame.revealEnabled = true;
    frame.spotlight = spotlight(SpotlightType::Circle);
    frame.spotlight.radius = {15, LengthUnit::Pixels};
    frame.spotlight.softness = {2, LengthUnit::Pixels};
    (void)renderer.render(frame);
    frame.destination = red.image();
    frame.destinationReady = false;
    frame.spotlight = {};
    frame.cursor = {80.5, 80.5};
    auto rendered = pixels(renderer.render(frame));
    colorAt(rendered, 100, 80, 80, {255, 255, 255}, "retained wallpaper cursor stays live while loading a none profile");
    colorAt(rendered, 100, 20, 20, {102, 102, 102}, "retained wallpaper old cursor is cleared");
    frame.spotlightEnabled = false;
    colorAt(pixels(renderer.render(frame)), 100, 20, 20, {255, 255, 255}, "pending wallpaper obeys immediate Spotlight off");
    frame.spotlightEnabled = true;
    colorAt(pixels(renderer.render(frame)), 100, 20, 20, {102, 102, 102}, "pending wallpaper restores Spotlight after on");
}

void glStateSurvivesRendering() {
    const ImageFixture white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    GLuint vao = 0, unpackBuffer = 0;
    std::array<GLuint, 3> samplers{};
    std::array<GLuint, 2> framebuffers{};
    const auto program = glCreateProgram();
    const std::array sources{
        std::pair{GL_VERTEX_SHADER, "#version 300 es\nvoid main() { gl_Position = vec4(0.0); }"},
        std::pair{GL_FRAGMENT_SHADER, "#version 300 es\nprecision mediump float; out vec4 color; void main() { color = vec4(1.0); }"},
    };
    for (const auto& [type, source] : sources) {
        const auto shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    require(linked, "test's host shader failed to link");
    glUseProgram(program);
    glGenVertexArrays(1, &vao);
    glGenSamplers(samplers.size(), samplers.data());
    glGenFramebuffers(framebuffers.size(), framebuffers.data());
    for (const auto framebuffer : framebuffers) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, white.id, 0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[0]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[1]);
    glBindVertexArray(vao);
    glViewport(2, 3, 7, 9);
    glScissor(1, 2, 3, 4);
    glEnable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_RASTERIZER_DISCARD);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glEnable(GL_SAMPLE_COVERAGE);
    glSampleCoverage(0, GL_TRUE);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    for (std::size_t unit = 0; unit < samplers.size(); ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, white.id);
        glBindSampler(unit, samplers[unit]);
    }
    glGenBuffers(1, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 16, nullptr, GL_STATIC_DRAW);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 13);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
    (void)renderer.render(frame);
    GLint actual = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &actual);
    require(actual == static_cast<GLint>(vao), "GPU renderer leaked its VAO");
    glGetIntegerv(GL_ACTIVE_TEXTURE, &actual);
    require(actual == GL_TEXTURE2, "GPU renderer leaked its active texture");
    for (std::size_t unit = 0; unit < samplers.size(); ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &actual);
        require(actual == static_cast<GLint>(white.id), "GPU renderer leaked a texture binding");
        glGetIntegerv(GL_SAMPLER_BINDING, &actual);
        require(actual == static_cast<GLint>(samplers[unit]), "GPU renderer leaked a sampler");
        glBindSampler(unit, 0);
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &actual);
    require(actual == static_cast<GLint>(program), "GPU renderer leaked its program");
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &actual);
    require(actual == static_cast<GLint>(framebuffers[0]), "GPU renderer leaked its draw framebuffer");
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &actual);
    require(actual == static_cast<GLint>(framebuffers[1]), "GPU renderer leaked its read framebuffer");
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &actual);
    require(actual == static_cast<GLint>(unpackBuffer), "GPU renderer leaked its unpack buffer");
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &actual);
    require(actual == 8, "GPU renderer leaked unpack alignment");
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &actual);
    require(actual == 13, "GPU renderer leaked unpack row length");
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &actual);
    require(actual == 2, "GPU renderer leaked unpack skip rows");
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &actual);
    require(actual == 3, "GPU renderer leaked unpack skip pixels");
    std::array<GLint, 4> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    require(viewport == std::array<GLint, 4>{2, 3, 7, 9}, "GPU renderer leaked its viewport");
    glGetIntegerv(GL_SCISSOR_BOX, viewport.data());
    require(viewport == std::array<GLint, 4>{1, 2, 3, 4}, "GPU renderer leaked its scissor");
    std::array<GLboolean, 4> colorMask{};
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask.data());
    require(colorMask == std::array<GLboolean, 4>{GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE}, "GPU renderer leaked its color mask");
    for (const auto cap : {GL_BLEND, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD,
                           GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE}) {
        require(glIsEnabled(cap), "GPU renderer leaked a GL capability");
        glDisable(cap);
    }
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glDeleteVertexArrays(1, &vao);
    glDeleteSamplers(samplers.size(), samplers.data());
    glDeleteFramebuffers(framebuffers.size(), framebuffers.data());
    glDeleteBuffers(1, &unpackBuffer);
    glDeleteProgram(program);
    require(glGetError() == GL_NO_ERROR, "GPU renderer produced a GL state error");
}

void invalidTargetsFailWithoutBreakingTheLastFrame() {
    const ImageFixture white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    const auto valid = renderer.render(frame);
    const auto draws = renderer.statistics().draws;
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    frame.pixelSize = {limit + 1.0, 80};
    bool rejected = false;
    try {
        (void)renderer.render(frame);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "oversized GPU target was not rejected safely");
    colorAt(pixels(valid), 100, 50, 40, {255, 255, 255}, "failed allocation preserves the last valid wallpaper");
    frame.pixelSize = {100, 80};
    require(renderer.render(frame)->id() == valid->id() && renderer.statistics().draws == draws, "GPU renderer did not recover after invalid target dimensions");
}

void repeatedSwitchesAndOutputResizeReleaseGpuMemory() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(red);
    (void)renderer.render(frame);
    frame.transitioning = true;
    frame.transition = transition();
    for (int i = 1; i <= 100; ++i) {
        frame.transitionId = i;
        frame.destination = i % 2 ? blue.image() : red.image();
        frame.progress = 0.5;
        (void)renderer.render(frame);
        require(renderer.statistics().residentBytes <= 3U * 100U * 80U * 4U + 64U, "interruptions accumulated GPU snapshot textures");
    }
    frame.transitioning = false;
    frame.progress = 1;
    (void)renderer.render(frame);
    require(renderer.statistics().residentBytes == 100U * 80U * 4U + 64U, "completed switches retained GPU snapshots");
    frame.logicalSize = {400, 600};
    frame.pixelSize = {120, 180};
    const auto resized = renderer.render(frame);
    require(resized->size() == Vec2{120, 180}, "rotated/fractionally scaled output has wrong target dimensions");
    colorAt(pixels(resized), 120, 30, 40, {255, 0, 0}, "resized output remains rendered");
}

void writeSpotlightPreviews(const std::string& directory, const int width, const int height, const double radius, const bool includeVariants) {
    std::vector<std::uint8_t> source(static_cast<std::size_t>(width * height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y * width + x) * 4;
            const auto u = static_cast<double>(x) / (width - 1);
            const auto v = static_cast<double>(y) / (height - 1);
            source[offset] = static_cast<std::uint8_t>(25 + 210 * u);
            source[offset + 1] = static_cast<std::uint8_t>(45 + 170 * (1.0 - v));
            source[offset + 2] = static_cast<std::uint8_t>(80 + 150 * (0.5 * u + 0.5 * v));
            source[offset + 3] = 255;
        }
    }

    const Vec2 dimensions{static_cast<double>(width), static_cast<double>(height)};
    ImageFixture wallpaper(dimensions, source);
    gpu::Renderer renderer;
    gpu::Input frame;
    frame.destination = wallpaper.image();
    frame.logicalSize = dimensions;
    frame.pixelSize = dimensions;
    frame.cursor = {dimensions.x * 0.5, dimensions.y * 0.5};
    frame.transitionOrigin = frame.cursor;
    frame.revealEnabled = true;
    frame.spotlight = spotlight(SpotlightType::Circle);
    frame.spotlight.radius = {radius, LengthUnit::Pixels};
    frame.spotlight.softness = {8, LengthUnit::Pixels};
    frame.spotlight.maskColor = {0.02, 0.04, 0.08};
    frame.spotlight.maskOpacity = 0.78;
    const auto circlePixels = pixels(renderer.render(frame));
    const auto sampleX = static_cast<int>(frame.cursor.x + frame.spotlight.radius.value - 12);
    const auto sampleY = static_cast<int>(frame.cursor.y);
    const auto sampleOffset = static_cast<std::size_t>(sampleY * width + sampleX) * 4;
    require(std::abs(static_cast<int>(circlePixels[sampleOffset]) - static_cast<int>(source[sampleOffset])) <= 2,
            "preview circle has a broad dim band inside the reveal");
    const auto circleName = includeVariants ? "spotlight-circle" : "spotlight-circle-" + std::to_string(width) + "x" + std::to_string(height);
    writePpm(directory + "/" + circleName + ".ppm", circlePixels, width, height);

    if (!includeVariants)
        return;

    frame.cursor = {dimensions.x * 0.72, dimensions.y * 0.43};
    frame.transitionOrigin = frame.cursor;
    writePpm(directory + "/spotlight-circle-moved.ppm", pixels(renderer.render(frame)), width, height);

    frame.spotlight = spotlight(SpotlightType::Fan);
    frame.spotlight.radius = {230, LengthUnit::Pixels};
    frame.spotlight.softness = {8, LengthUnit::Pixels};
    frame.spotlight.anchor = {0.5, 0.5};
    frame.spotlight.beamStartReveal = 0.2;
    writePpm(directory + "/spotlight-fan.ppm", pixels(renderer.render(frame)), width, height);
}

void resizePreservesActiveAndPendingTransitions() {
    const ImageFixture red{{255, 0, 0, 255}}, blue{{0, 0, 255, 255}}, green{{0, 255, 0, 255}};
    gpu::Renderer renderer;
    auto frame = input(red);
    (void)renderer.render(frame);
    frame.destination = blue.image();
    frame.transition = transition();
    frame.transitioning = true;
    frame.transitionId = 1;
    frame.progress = 0.5;
    (void)renderer.render(frame);
    frame.pixelSize = {120, 96};
    colorAt(pixels(renderer.render(frame)), 120, 50, 40, {128, 0, 128}, "resize must not discard an active transition's source");
    colorAt(pixels(renderer.render(frame)), 120, 50, 40, {128, 0, 128}, "post-resize frame must not fade from an empty source");
    frame.destination = green.image();
    frame.transitionId = 2;
    frame.progress = 0;
    frame.destinationReady = false;
    frame.pixelSize = {150, 120};
    frame.logicalSize = {800, 640};
    colorAt(pixels(renderer.render(frame)), 150, 50, 40, {128, 0, 128}, "resize retains visible wallpaper while destination decoding is pending");
    frame.destinationReady = true;
    frame.progress = 0.5;
    colorAt(pixels(renderer.render(frame)), 150, 50, 40, {64, 128, 64}, "resized pending transition starts from the retained source");
}

void fourKShaderAndBoundedInterruptedMasks() {
    const ImageFixture white{{255, 255, 255, 255}};
    gpu::Renderer renderer;
    auto frame = input(white);
    frame.logicalSize = frame.pixelSize = {3840, 2160};
    frame.cursor = {1920.5, 1080.5};
    frame.revealEnabled = true;
    frame.spotlight = spotlight(SpotlightType::Circle);
    frame.spotlight.radius = {18, LengthUnit::ShortEdgePercent};
    frame.spotlight.softness = {4, LengthUnit::ShortEdgePercent};
    frame.transitionOrigin = frame.cursor;
    auto rendered = pixels(renderer.render(frame));
    colorAt(rendered, 3840, 1920, 1080, {255, 255, 255}, "4K shader cursor center");
    colorAt(rendered, 3840, 1, 1, {102, 102, 102}, "4K shader mask exterior");
    const auto firstPixels = renderer.statistics().pixelsDrawn;
    frame.cursor = {3480.5, 1080.5};
    frame.transitionOrigin = frame.cursor;
    rendered = pixels(renderer.render(frame));
    require(renderer.statistics().pixelsDrawn - firstPixels < 3840U * 2160U, "unused cursor transition origin turned a local Spotlight update into a full 4K redraw");
    colorAt(rendered, 3840, 3480, 1080, {255, 255, 255}, "4K partial update follows the cursor");
    colorAt(rendered, 3840, 1920, 1080, {102, 102, 102}, "4K partial update clears the previous cursor");

    // Stress parameter recipes at small resolution; this is not a performance benchmark.
    frame.logicalSize = frame.pixelSize = {100, 80};
    frame.cursor = {50.5, 40.5};
    frame.spotlight = spotlight(SpotlightType::Circle);
    (void)renderer.render(frame);
    frame.transitioning = true;
    frame.transition = transition();
    for (int i = 1; i <= 100; ++i) {
        frame.transitionId = i;
        frame.progress = 0.5;
        frame.spotlight = spotlight(static_cast<SpotlightType>(i % 4));
        (void)renderer.render(frame);
        require(renderer.statistics().residentBytes <= 100U * 80U * 4U + 4U * 64U, "interrupted masks grew without bound across fixed workspace profiles");
    }
    require(renderer.statistics().snapshots == 0, "mask-only interruptions must not snapshot wallpaper");
    frame.transitioning = false;
    frame.progress = 1;
    (void)renderer.render(frame);
    require(renderer.statistics().residentBytes == 100U * 80U * 4U + 64U, "completed mask transitions did not release extra GPU parameters");
}

} // namespace

int main() {
    try {
        EglContext context;
        spotlightPixelsAndReferenceGeometry();
        fitFocalPointAlphaAndImageOrientation();
        transitionsUseShadersAndEasing();
        clockOriginIsDefinedForEveryDriver();
        repeatedInterruptionsCaptureWithoutQueueing();
        geometricInterruptionsPreserveEveryPixel();
        cursorRemainsLiveAcrossMaskInterruptions();
        engineKeepsInterruptedRevealLiveWhenLatestProfilesUseNone();
        sameTypeParametersAndSpotlightOverride();
        fractionalScaleDamageAndIndependentOutputs();
        pendingWallpaperRetainsVisibleSourceAndIdleStopsDrawing();
        pendingWallpaperKeepsCursorAndTemporaryOverridesLive();
        glStateSurvivesRendering();
        invalidTargetsFailWithoutBreakingTheLastFrame();
        repeatedSwitchesAndOutputResizeReleaseGpuMemory();
        resizePreservesActiveAndPendingTransitions();
        fourKShaderAndBoundedInterruptedMasks();
        if (const auto* previewDirectory = std::getenv("LUXAXIS_GPU_PREVIEW_DIR")) {
            writeSpotlightPreviews(previewDirectory, 800, 450, 180, true);
            writeSpotlightPreviews(previewDirectory, 1920, 1680, 672, false);
            writeSpotlightPreviews(previewDirectory, 1920, 1080, 60, false);
        }
    } catch (const std::exception& error) {
        std::cerr << "gpu_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "gpu_test: all checks passed\n";
    return EXIT_SUCCESS;
}
