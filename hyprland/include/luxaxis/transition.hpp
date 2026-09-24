#pragma once

#include "luxaxis/types.hpp"

namespace luxaxis {

[[nodiscard]] double easeProgress(double progress, std::string_view easing);
[[nodiscard]] double transitionReveal(const Transition& transition, double progress, Vec2 point, Vec2 outputSize, Vec2 origin);

} // namespace luxaxis
