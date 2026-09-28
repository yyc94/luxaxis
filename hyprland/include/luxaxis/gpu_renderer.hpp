#pragma once

#include "luxaxis/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace luxaxis::gpu {

struct Image {
    std::uint32_t texture = 0;
    Vec2 size{};
    FitMode fit = FitMode::Cover;
    Vec2 position{0.5, 0.5};
    std::shared_ptr<void> lifetime;

    friend bool operator==(const Image&, const Image&) = default;
};

struct Input {
    Image destination;
    Spotlight spotlight;
    Color fallbackColor{};
    Vec2 logicalSize;
    Vec2 pixelSize;
    Vec2 cursor;
    Vec2 transitionOrigin;
    Transition transition;
    double progress = 1.0;
    std::uint64_t transitionId = 0;
    bool transitioning = false;
    bool revealEnabled = false;
    bool spotlightEnabled = true;
    bool destinationReady = true;
};

struct RendererImpl;

class Texture {
  public:
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] std::uint32_t id() const { return texture_; }
    [[nodiscard]] Vec2 size() const { return size_; }

  private:
    explicit Texture(Vec2 size);
    std::uint32_t texture_ = 0;
    std::uint32_t framebuffer_ = 0;
    Vec2 size_;
    friend struct RendererImpl;
};

struct Statistics {
    std::uint64_t draws = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t pixelsDrawn = 0;
    std::size_t residentBytes = 0;
};

// Rendering and destruction require the caller's GLES 3 context to be current;
// damage and statistics queries do not access GL.
// The returned opaque sRGB texture uses top-left image coordinates. All GL state
// touched by this module is restored before returning to the compositor.
class Renderer {
  public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] std::shared_ptr<const Texture> render(const Input& input);
    // Union of the last drawn and new cursor's live mask bounds, in logical coordinates.
    [[nodiscard]] std::optional<Rect> cursorDamage(Vec2 cursor) const;
    [[nodiscard]] Statistics statistics() const;

  private:
    std::unique_ptr<RendererImpl> impl_;
};

} // namespace luxaxis::gpu
