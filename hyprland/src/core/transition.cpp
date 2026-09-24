#include "luxaxis/transition.hpp"

#include <algorithm>
#include <cmath>

namespace luxaxis {
namespace {

constexpr double PI = 3.14159265358979323846;

double distance(Vec2 a, Vec2 b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

} // namespace

double easeProgress(const double progress, const std::string_view easing) {
    const auto p = std::clamp(progress, 0.0, 1.0);
    if (easing == "ease-in")
        return p * p * p;
    if (easing == "ease-out")
        return 1.0 - (1.0 - p) * (1.0 - p) * (1.0 - p);
    if (easing == "ease-in-out")
        return p < 0.5 ? 4.0 * p * p * p : 1.0 - std::pow(-2.0 * p + 2.0, 3.0) / 2.0;
    return p;
}

double transitionReveal(const Transition& transition, const double progress, const Vec2 point, const Vec2 outputSize, const Vec2 origin) {
    const auto p = easeProgress(progress, transition.easing);
    if (transition.type == TransitionType::None || p >= 1.0)
        return 1.0;
    if (p <= 0.0)
        return 0.0;

    const auto maxRadius = std::max({
        distance(origin, {0.0, 0.0}),
        distance(origin, {outputSize.x, 0.0}),
        distance(origin, {0.0, outputSize.y}),
        distance(origin, outputSize),
    });
    switch (transition.type) {
        case TransitionType::Fade:
            return p;
        case TransitionType::Wipe: {
            const auto fromRight = origin.x > outputSize.x * 0.5;
            const auto edge = fromRight ? outputSize.x * (1.0 - p) : outputSize.x * p;
            return fromRight ? (point.x >= edge ? 1.0 : 0.0) : (point.x <= edge ? 1.0 : 0.0);
        }
        case TransitionType::Grow:
            return distance(point, origin) <= maxRadius * p ? 1.0 : 0.0;
        case TransitionType::Outer: {
            const auto left = origin.x * p;
            const auto right = origin.x + (outputSize.x - origin.x) * (1.0 - p);
            const auto top = origin.y * p;
            const auto bottom = origin.y + (outputSize.y - origin.y) * (1.0 - p);
            return point.x <= left || point.x >= right || point.y <= top || point.y >= bottom ? 1.0 : 0.0;
        }
        case TransitionType::Clock: {
            const auto angle = std::atan2(point.y - origin.y, point.x - origin.x) + PI * 0.5;
            const auto normalized = std::fmod(angle + 2.0 * PI, 2.0 * PI) / (2.0 * PI);
            return normalized <= p ? 1.0 : 0.0;
        }
        case TransitionType::Random:
        case TransitionType::None:
            return p;
    }
    return p;
}

} // namespace luxaxis
