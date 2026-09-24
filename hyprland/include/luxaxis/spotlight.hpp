#pragma once

#include "luxaxis/types.hpp"

#include <optional>

namespace luxaxis {

struct SpotlightSample {
    Spotlight spotlight;
    Vec2 outputSize;
    Vec2 cursor;
    bool revealEnabled = true;
    Vec2 fanDirection{0.0, 1.0};
};

[[nodiscard]] Vec2 resolveFanDirection(Vec2 anchor, Vec2 cursor, Vec2 previousDirection);
[[nodiscard]] std::optional<Rect> spotlightEffectBounds(const SpotlightSample& sample);
[[nodiscard]] double revealAt(const SpotlightSample& sample, Vec2 point);
[[nodiscard]] double maskAlphaAt(const SpotlightSample& sample, Vec2 point);
[[nodiscard]] Color applySpotlightMask(Color wallpaper, const SpotlightSample& sample, Vec2 point);

} // namespace luxaxis
