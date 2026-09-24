#include "luxaxis/engine.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

luxaxis::Config config() {
    luxaxis::Config result;
    result.defaultProfile = "default";
    result.profiles.emplace("default", luxaxis::Profile{.wallpaper = "/wall/default.webp"});
    result.profiles.emplace(
        "focus",
        luxaxis::Profile{
            .wallpaper = "/wall/focus.png",
            .spotlight = {
                .type = luxaxis::SpotlightType::Circle,
                .maskOpacity = 0.5,
                .radius = {20.0, luxaxis::LengthUnit::ShortEdgePercent},
                .softness = {20.0, luxaxis::LengthUnit::Pixels},
            },
        });
    result.workspaces.emplace(2, "focus");
    return result;
}

luxaxis::Engine twoOutputs() {
    luxaxis::Engine engine{config()};
    require(engine.upsertOutput({"left", {{0.0, 0.0}, {1920.0, 1080.0}}, 2}), "left output was not added");
    require(engine.upsertOutput({"right", {{1920.0, 0.0}, {2560.0, 1440.0}}, 1}), "right output was not added");
    return engine;
}

void profilesResolvePerOutput() {
    auto engine = twoOutputs();
    const auto left = engine.planFor("left");
    const auto right = engine.planFor("right");
    require(left && left->profileName == "focus", "mapped workspace did not resolve its profile");
    require(right && right->profileName == "default", "unmapped workspace did not use the default profile");
    require(left->wallpaperCandidates.size() == 2 && left->wallpaperCandidates[1] == "/wall/default.webp", "default image fallback was not planned");
    require(right->wallpaperCandidates.size() == 1, "default profile redundantly fell back to itself");
}

void cursorPolicySelectsOnlyOneReveal() {
    auto engine = twoOutputs();
    require(engine.setCursor({100.0, 200.0}), "cursor did not move");
    require(engine.activeOutput() == "left", "cursor-containing output was not active");
    require(engine.planFor("left")->revealEnabled, "active Spotlight did not reveal");
    require(!engine.planFor("right")->maskEnabled, "none profile gained a mask");

    require(engine.activateWorkspace("right", 2), "right workspace did not change");
    const auto right = engine.planFor("right");
    require(right->maskEnabled && !right->revealEnabled, "non-active output did not retain only its mask");

    require(engine.setCursor({2200.0, 300.0}), "cursor did not enter right output");
    require(engine.planFor("right")->revealEnabled, "reveal did not transfer to the cursor output");
    require(!engine.planFor("left")->revealEnabled, "old cursor output retained a reveal");
}

void focusedPolicyAllowsCursorOutsideOutput() {
    auto settings = config();
    settings.activeOutput = luxaxis::ActiveOutputPolicy::Focused;
    luxaxis::Engine engine{std::move(settings)};
    require(engine.upsertOutput({"left", {{0.0, 0.0}, {1000.0, 800.0}}, 2}), "output was not added");
    require(engine.setCursor({2000.0, 500.0}), "cursor did not move");
    require(engine.setFocusedOutput("left"), "focused output did not change");
    const auto plan = engine.planFor("left");
    require(plan->revealEnabled, "focused output did not receive the reveal");
    require(plan->cursorLocal == luxaxis::Vec2{2000.0, 500.0}, "off-output cursor was incorrectly clamped");
}

void invalidWorkspaceEventsDoNotChangeNormalState() {
    auto engine = twoOutputs();
    const auto revision = engine.revision();
    require(!engine.activateWorkspace("left", 0), "zero workspace was accepted");
    require(!engine.activateWorkspace("left", -99), "special workspace was accepted");
    require(engine.revision() == revision, "ignored workspace changed engine state");
    require(engine.planFor("left")->workspace == 2, "ignored workspace replaced the normal workspace");
}

void exclusionsAndSessionOverrideAreApplied() {
    auto settings = config();
    settings.excludedOutputs.insert("right");
    luxaxis::Engine engine{std::move(settings)};
    require(engine.upsertOutput({"left", {{0.0, 0.0}, {1000.0, 800.0}}, 2}), "left output was not added");
    require(engine.upsertOutput({"right", {{1000.0, 0.0}, {1000.0, 800.0}}, 2}), "right output was not added");
    require(engine.setCursor({100.0, 100.0}), "cursor did not move");
    require(!engine.planFor("right"), "excluded output produced a render plan");

    engine.spotlightOff();
    auto left = engine.planFor("left");
    require(!left->maskEnabled && !left->revealEnabled && left->profile.spotlight.type == luxaxis::SpotlightType::None, "Spotlight off did not behave as none");
    engine.spotlightToggle();
    require(engine.planFor("left")->maskEnabled, "Spotlight toggle did not restore profile behavior");
}

void configReplacementReplansExistingOutputs() {
    auto engine = twoOutputs();
    auto replacement = config();
    replacement.workspaces[2] = "default";
    engine.applyConfig(std::move(replacement));
    require(engine.planFor("left")->profileName == "default", "configuration replacement did not replan existing output");
}

void ignoredWorkspaceKeepsNormalProfile() {
    auto engine = twoOutputs();
    require(!engine.upsertOutput({"left", {{0.0, 0.0}, {1920.0, 1080.0}}, std::nullopt}), "ignored workspace caused an unnecessary output update");
    require(engine.planFor("left")->workspace == 2, "ignored workspace cleared the normal workspace");
    require(engine.planFor("left")->profileName == "focus", "ignored workspace changed the active profile");
}

} // namespace

int main() {
    try {
        profilesResolvePerOutput();
        cursorPolicySelectsOnlyOneReveal();
        focusedPolicyAllowsCursorOutsideOutput();
        invalidWorkspaceEventsDoNotChangeNormalState();
        exclusionsAndSessionOverrideAreApplied();
        configReplacementReplansExistingOutputs();
        ignoredWorkspaceKeepsNormalProfile();
    } catch (const std::exception& error) {
        std::cerr << "engine_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "engine_test: all checks passed\n";
    return EXIT_SUCCESS;
}
