#include "luxaxis/spotlight.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void near(const double actual, const double expected, const double tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message + ": got " + std::to_string(actual));
}

luxaxis::Spotlight base(const luxaxis::SpotlightType type) {
    return {
        .type = type,
        .maskColor = {0.0, 0.0, 0.0},
        .maskOpacity = 0.6,
        .radius = {20.0, luxaxis::LengthUnit::ShortEdgePercent},
        .softness = {20.0, luxaxis::LengthUnit::Pixels},
        .orientation = luxaxis::StripOrientation::Horizontal,
        .thickness = {200.0, luxaxis::LengthUnit::Pixels},
        .anchor = {0.5, 0.0},
        .aspectRatio = 0.5,
        .beamStartReveal = 0.25,
    };
}

void noneNeverMasks() {
    luxaxis::SpotlightSample sample{.spotlight = base(luxaxis::SpotlightType::None), .outputSize = {1000.0, 800.0}, .cursor = {500.0, 400.0}, .revealEnabled = false};
    near(luxaxis::maskAlphaAt(sample, {0.0, 0.0}), 0.0, 0.0001, "none Spotlight added a mask");
}

void circleHasStableSoftBoundary() {
    luxaxis::SpotlightSample sample{.spotlight = base(luxaxis::SpotlightType::Circle), .outputSize = {1000.0, 800.0}, .cursor = {500.0, 400.0}};
    near(luxaxis::revealAt(sample, {500.0, 400.0}), 1.0, 0.0001, "circle center was not revealed");
    near(luxaxis::revealAt(sample, {650.0, 400.0}), 0.5, 0.0001, "circle softness band was incorrect");
    near(luxaxis::revealAt(sample, {660.0, 400.0}), 0.0, 0.0001, "circle outer boundary leaked");
    near(luxaxis::maskAlphaAt(sample, {900.0, 400.0}), 0.6, 0.0001, "circle exterior was not fully masked");
}

void stripSpansOutputOnItsLongAxis() {
    auto horizontal = base(luxaxis::SpotlightType::Strip);
    luxaxis::SpotlightSample sample{.spotlight = horizontal, .outputSize = {1000.0, 800.0}, .cursor = {500.0, 400.0}};
    near(luxaxis::revealAt(sample, {0.0, 400.0}), 1.0, 0.0001, "horizontal strip did not span output");
    near(luxaxis::revealAt(sample, {900.0, 500.0}), 0.0, 0.0001, "horizontal strip exceeded its thickness");

    sample.spotlight.orientation = luxaxis::StripOrientation::Vertical;
    near(luxaxis::revealAt(sample, {500.0, 0.0}), 1.0, 0.0001, "vertical strip did not span output");
    near(luxaxis::revealAt(sample, {600.0, 700.0}), 0.0, 0.0001, "vertical strip exceeded its thickness");
}

void fanIsTriangleUnionCursorEllipse() {
    luxaxis::SpotlightSample sample{.spotlight = base(luxaxis::SpotlightType::Fan), .outputSize = {1000.0, 800.0}, .cursor = {500.0, 400.0}};
    near(luxaxis::revealAt(sample, sample.cursor), 1.0, 0.0001, "fan cursor ellipse did not reveal its center");
    require(luxaxis::revealAt(sample, {500.0, 200.0}) > 0.5, "fan triangle did not connect anchor to ellipse");
    near(luxaxis::revealAt(sample, {250.0, 200.0}), 0.0, 0.0001, "fan revealed outside the triangle");
    near(luxaxis::revealAt(sample, {500.0, 490.0}), 0.0, 0.0001, "fan continued beyond its cursor ellipse");

    sample.spotlight.anchor = {0.0, 0.5};
    sample.cursor = {600.0, 400.0};
    require(luxaxis::revealAt(sample, {300.0, 400.0}) > 0.5, "rotated fan triangle was not aligned to its anchor");
    near(luxaxis::revealAt(sample, {300.0, 100.0}), 0.0, 0.0001, "rotated fan revealed outside its transverse boundary");
}

void fanDegeneracyPreservesDirection() {
    const auto direction = luxaxis::resolveFanDirection({10.0, 10.0}, {10.0, 10.0}, {1.0, 0.0});
    near(direction.x, 1.0, 0.0001, "degenerate fan discarded its prior direction");
    near(direction.y, 0.0, 0.0001, "degenerate fan prior direction changed");

    auto spotlight = base(luxaxis::SpotlightType::Fan);
    spotlight.anchor = {0.5, 0.5};
    luxaxis::SpotlightSample sample{
        .spotlight = spotlight,
        .outputSize = {1000.0, 800.0},
        .cursor = {500.0, 400.0},
        .fanDirection = {1.0, 0.0},
    };
    near(luxaxis::revealAt(sample, sample.cursor), 1.0, 0.0001, "degenerate fan lost its ellipse");
}

void inactiveOutputKeepsMaskWithoutReveal() {
    luxaxis::SpotlightSample sample{.spotlight = base(luxaxis::SpotlightType::Circle), .outputSize = {1000.0, 800.0}, .cursor = {500.0, 400.0}, .revealEnabled = false};
    near(luxaxis::maskAlphaAt(sample, sample.cursor), 0.6, 0.0001, "inactive output retained a reveal");
    const auto shaded = luxaxis::applySpotlightMask({1.0, 0.5, 0.25}, sample, sample.cursor);
    near(shaded.r, 0.4, 0.0001, "mask color blend was incorrect");
    near(shaded.g, 0.2, 0.0001, "mask color blend was incorrect");
    near(shaded.b, 0.1, 0.0001, "mask color blend was incorrect");
}

} // namespace

int main() {
    try {
        noneNeverMasks();
        circleHasStableSoftBoundary();
        stripSpansOutputOnItsLongAxis();
        fanIsTriangleUnionCursorEllipse();
        fanDegeneracyPreservesDirection();
        inactiveOutputKeepsMaskWithoutReveal();
    } catch (const std::exception& error) {
        std::cerr << "spotlight_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "spotlight_test: all checks passed\n";
    return EXIT_SUCCESS;
}
