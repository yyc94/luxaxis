#include "luxaxis/spotlight.hpp"

#include <algorithm>
#include <cmath>

namespace luxaxis {
namespace {

constexpr double EPSILON = 0.0001;

Vec2 subtract(const Vec2 lhs, const Vec2 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y};
}

Vec2 add(const Vec2 lhs, const Vec2 rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y};
}

Vec2 multiply(const Vec2 value, const double factor) {
    return {value.x * factor, value.y * factor};
}

double dot(const Vec2 lhs, const Vec2 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y;
}

double lengthOf(const Vec2 value) {
    return std::hypot(value.x, value.y);
}

Vec2 normalizeOr(const Vec2 value, const Vec2 fallback) {
    const auto magnitude = lengthOf(value);
    return magnitude > EPSILON ? multiply(value, 1.0 / magnitude) : fallback;
}

double edgeReveal(const double signedDistance, const double softness) {
    if (softness <= EPSILON)
        return signedDistance <= 0.0 ? 1.0 : 0.0;
    return std::clamp(-signedDistance / softness, 0.0, 1.0);
}

double segmentDistance(const Vec2 point, const Vec2 start, const Vec2 end) {
    const auto segment = subtract(end, start);
    const auto lengthSquared = dot(segment, segment);
    if (lengthSquared <= EPSILON * EPSILON)
        return lengthOf(subtract(point, start));
    const auto position = std::clamp(dot(subtract(point, start), segment) / lengthSquared, 0.0, 1.0);
    return lengthOf(subtract(point, add(start, multiply(segment, position))));
}

double cross(const Vec2 lhs, const Vec2 rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

bool pointInTriangle(const Vec2 point, const Vec2 a, const Vec2 b, const Vec2 c) {
    const auto ab = cross(subtract(b, a), subtract(point, a));
    const auto bc = cross(subtract(c, b), subtract(point, b));
    const auto ca = cross(subtract(a, c), subtract(point, c));
    const bool hasNegative = ab < 0.0 || bc < 0.0 || ca < 0.0;
    const bool hasPositive = ab > 0.0 || bc > 0.0 || ca > 0.0;
    return !(hasNegative && hasPositive);
}

double triangleSignedDistance(const Vec2 point, const Vec2 a, const Vec2 b, const Vec2 c) {
    const auto distance = std::min({segmentDistance(point, a, b), segmentDistance(point, b, c), segmentDistance(point, c, a)});
    return pointInTriangle(point, a, b, c) ? -distance : distance;
}

Rect clippedBounds(const Vec2 minimum, const Vec2 maximum, const Vec2 outputSize) {
    const auto left = std::clamp(minimum.x, 0.0, outputSize.x);
    const auto top = std::clamp(minimum.y, 0.0, outputSize.y);
    const auto right = std::clamp(maximum.x, 0.0, outputSize.x);
    const auto bottom = std::clamp(maximum.y, 0.0, outputSize.y);
    return {{left, top}, {std::max(0.0, right - left), std::max(0.0, bottom - top)}};
}

double circleReveal(const SpotlightSample& sample, const Vec2 point) {
    const auto shortEdge = std::min(sample.outputSize.x, sample.outputSize.y);
    const auto radius = sample.spotlight.radius.resolve(shortEdge);
    const auto softness = sample.spotlight.softness.resolve(shortEdge);
    return edgeReveal(lengthOf(subtract(point, sample.cursor)) - radius, softness);
}

double stripReveal(const SpotlightSample& sample, const Vec2 point) {
    const auto shortEdge = std::min(sample.outputSize.x, sample.outputSize.y);
    const auto halfThickness = sample.spotlight.thickness.resolve(shortEdge) * 0.5;
    const auto softness = sample.spotlight.softness.resolve(shortEdge);
    const auto distance = sample.spotlight.orientation == StripOrientation::Horizontal ? std::abs(point.y - sample.cursor.y) : std::abs(point.x - sample.cursor.x);
    return edgeReveal(distance - halfThickness, softness);
}

double fanReveal(const SpotlightSample& sample, const Vec2 point) {
    const auto shortEdge = std::min(sample.outputSize.x, sample.outputSize.y);
    const auto radius = sample.spotlight.radius.resolve(shortEdge);
    const auto longitudinalRadius = radius * sample.spotlight.aspectRatio;
    const auto softness = sample.spotlight.softness.resolve(shortEdge);
    const Vec2 anchor{
        sample.spotlight.anchor.x * sample.outputSize.x,
        sample.spotlight.anchor.y * sample.outputSize.y,
    };
    const auto direction = resolveFanDirection(anchor, sample.cursor, sample.fanDirection);
    const Vec2 transverse{-direction.y, direction.x};

    const auto relative = subtract(point, sample.cursor);
    const auto along = dot(relative, direction);
    const auto across = dot(relative, transverse);
    const auto ellipseDistance = std::hypot(along / longitudinalRadius, across / radius);
    const auto ellipseSignedDistance = (ellipseDistance - 1.0) * std::min(radius, longitudinalRadius);
    const auto ellipse = edgeReveal(ellipseSignedDistance, softness);

    const auto anchorDistance = lengthOf(subtract(sample.cursor, anchor));
    if (anchorDistance <= EPSILON)
        return ellipse;

    const auto first = add(sample.cursor, multiply(transverse, radius));
    const auto second = subtract(sample.cursor, multiply(transverse, radius));
    const auto shape = edgeReveal(triangleSignedDistance(point, anchor, first, second), softness);
    const auto progress = std::clamp(dot(subtract(point, anchor), direction) / anchorDistance, 0.0, 1.0);
    const auto beamStrength = std::lerp(sample.spotlight.beamStartReveal, 1.0, progress);
    return std::max(ellipse, shape * beamStrength);
}

} // namespace

Vec2 resolveFanDirection(const Vec2 anchor, const Vec2 cursor, const Vec2 previousDirection) {
    const auto delta = subtract(cursor, anchor);
    if (lengthOf(delta) > EPSILON)
        return normalizeOr(delta, {0.0, 1.0});
    return normalizeOr(previousDirection, {0.0, 1.0});
}

std::optional<Rect> spotlightEffectBounds(const SpotlightSample& sample) {
    if (sample.spotlight.type == SpotlightType::None || !sample.revealEnabled || sample.outputSize.x <= 0.0 || sample.outputSize.y <= 0.0)
        return std::nullopt;

    const auto shortEdge = std::min(sample.outputSize.x, sample.outputSize.y);
    const auto softness = sample.spotlight.softness.resolve(shortEdge);
    Vec2 minimum = sample.cursor;
    Vec2 maximum = sample.cursor;
    switch (sample.spotlight.type) {
        case SpotlightType::Circle: {
            const auto extent = sample.spotlight.radius.resolve(shortEdge) + softness;
            minimum = {sample.cursor.x - extent, sample.cursor.y - extent};
            maximum = {sample.cursor.x + extent, sample.cursor.y + extent};
            break;
        }
        case SpotlightType::Strip: {
            const auto extent = sample.spotlight.thickness.resolve(shortEdge) * 0.5 + softness;
            if (sample.spotlight.orientation == StripOrientation::Horizontal) {
                minimum = {0.0, sample.cursor.y - extent};
                maximum = {sample.outputSize.x, sample.cursor.y + extent};
            } else {
                minimum = {sample.cursor.x - extent, 0.0};
                maximum = {sample.cursor.x + extent, sample.outputSize.y};
            }
            break;
        }
        case SpotlightType::Fan: {
            const auto radius = sample.spotlight.radius.resolve(shortEdge);
            const auto longitudinalRadius = radius * sample.spotlight.aspectRatio;
            const auto anchor = Vec2{
                sample.spotlight.anchor.x * sample.outputSize.x,
                sample.spotlight.anchor.y * sample.outputSize.y,
            };
            // The direction can change between the damage event and the next
            // render. Use the orientation-independent enclosing circle here so
            // the damage never misses a newly rotated ellipse.
            const auto ellipseExtent = std::max(radius, longitudinalRadius);
            minimum = {
                std::min(anchor.x, sample.cursor.x - ellipseExtent) - softness,
                std::min(anchor.y, sample.cursor.y - ellipseExtent) - softness,
            };
            maximum = {
                std::max(anchor.x, sample.cursor.x + ellipseExtent) + softness,
                std::max(anchor.y, sample.cursor.y + ellipseExtent) + softness,
            };
            break;
        }
        case SpotlightType::None:
            return std::nullopt;
    }

    const auto result = clippedBounds(minimum, maximum, sample.outputSize);
    return result.size.x > 0.0 && result.size.y > 0.0 ? std::optional{result} : std::nullopt;
}

double revealAt(const SpotlightSample& sample, const Vec2 point) {
    if (sample.spotlight.type == SpotlightType::None)
        return 1.0;
    if (!sample.revealEnabled)
        return 0.0;

    switch (sample.spotlight.type) {
        case SpotlightType::None: return 1.0;
        case SpotlightType::Circle: return circleReveal(sample, point);
        case SpotlightType::Strip: return stripReveal(sample, point);
        case SpotlightType::Fan: return fanReveal(sample, point);
    }
    return 0.0;
}

double maskAlphaAt(const SpotlightSample& sample, const Vec2 point) {
    if (sample.spotlight.type == SpotlightType::None)
        return 0.0;
    return sample.spotlight.maskOpacity * (1.0 - revealAt(sample, point));
}

Color applySpotlightMask(const Color wallpaper, const SpotlightSample& sample, const Vec2 point) {
    const auto alpha = maskAlphaAt(sample, point);
    return {
        std::lerp(wallpaper.r, sample.spotlight.maskColor.r, alpha),
        std::lerp(wallpaper.g, sample.spotlight.maskColor.g, alpha),
        std::lerp(wallpaper.b, sample.spotlight.maskColor.b, alpha),
    };
}

} // namespace luxaxis
