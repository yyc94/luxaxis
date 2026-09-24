#include "luxaxis/engine.hpp"
#include "luxaxis/transition.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

luxaxis::Transition transition(luxaxis::TransitionType type) {
    return {.type = type, .durationMs = 100, .easing = "linear", .origin = luxaxis::TransitionOrigin::Center};
}

luxaxis::Transition randomTransition() {
    auto result = transition(luxaxis::TransitionType::Random);
    result.randomAllowlist = {luxaxis::TransitionType::Wipe, luxaxis::TransitionType::Grow};
    return result;
}

void allTransitionTypesReachDestination() {
    for (const auto type : {luxaxis::TransitionType::Fade, luxaxis::TransitionType::Wipe, luxaxis::TransitionType::Grow, luxaxis::TransitionType::Outer,
                            luxaxis::TransitionType::Clock}) {
        const auto spec = transition(type);
        require(luxaxis::transitionReveal(spec, 0.0, {10.0, 10.0}, {100.0, 100.0}, {50.0, 50.0}) == 0.0, "transition did not start hidden");
        require(luxaxis::transitionReveal(spec, 1.0, {10.0, 10.0}, {100.0, 100.0}, {50.0, 50.0}) == 1.0, "transition did not complete");
    }
}

void easingIsBounded() {
    require(luxaxis::easeProgress(0.5, "ease-in") < 0.5, "ease-in was not accelerating");
    require(luxaxis::easeProgress(0.5, "ease-out") > 0.5, "ease-out was not decelerating");
    require(luxaxis::easeProgress(-1.0, "linear") == 0.0 && luxaxis::easeProgress(2.0, "linear") == 1.0, "easing was not bounded");
}

void engineInterruptsInsteadOfQueueing() {
    luxaxis::Config config;
    config.defaultProfile = "default";
    config.profiles.emplace("default", luxaxis::Profile{.wallpaper = "/wall/default.png"});
    auto destination = luxaxis::Profile{.wallpaper = "/wall/one.png", .transition = transition(luxaxis::TransitionType::Fade)};
    config.profiles.emplace("one", destination);
    config.profiles.emplace("two", luxaxis::Profile{.wallpaper = "/wall/two.png", .transition = transition(luxaxis::TransitionType::Wipe)});
    config.workspaces.emplace(1, "one");
    config.workspaces.emplace(2, "two");

    luxaxis::Engine engine{config};
    require(engine.upsertOutput({"DP-1", {{0.0, 0.0}, {1000.0, 800.0}}, std::nullopt}), "output was not added");
    require(engine.activateWorkspace("DP-1", 1), "first workspace did not activate");
    engine.advance(std::chrono::milliseconds{40});
    const auto first = engine.planFor("DP-1");
    require(first->transitioning && first->transitionProgress > 0.0 && first->transitionProgress < 1.0, "first transition did not advance");
    require(engine.activateWorkspace("DP-1", 2), "second workspace did not activate");
    const auto interrupted = engine.planFor("DP-1");
    require(interrupted->transition.type == luxaxis::TransitionType::Wipe, "destination did not own interrupted transition");
    require(interrupted->previousProfile && interrupted->previousProfile->wallpaper == "/wall/one.png", "interruption did not capture current destination");
    engine.advance(std::chrono::milliseconds{100});
    require(!engine.planFor("DP-1")->transitioning, "transition did not complete");
}

void randomTransitionsAvoidImmediateRepeats() {
    luxaxis::Config config;
    config.defaultProfile = "default";
    config.profiles.emplace("default", luxaxis::Profile{.wallpaper = "/wall/default.png"});
    config.profiles.emplace("one", luxaxis::Profile{.wallpaper = "/wall/one.png", .transition = randomTransition()});
    config.profiles.emplace("two", luxaxis::Profile{.wallpaper = "/wall/two.png", .transition = randomTransition()});
    config.workspaces.emplace(1, "one");
    config.workspaces.emplace(2, "two");

    luxaxis::Engine engine{config};
    require(engine.upsertOutput({"DP-1", {{0.0, 0.0}, {1000.0, 800.0}}, std::nullopt}), "output was not added");
    require(engine.activateWorkspace("DP-1", 1), "first workspace did not activate");
    const auto first = engine.planFor("DP-1")->transition.type;
    engine.advance(std::chrono::milliseconds{100});
    require(engine.activateWorkspace("DP-1", 2), "second workspace did not activate");
    const auto second = engine.planFor("DP-1")->transition.type;
    require(first != second, "random transition repeated immediately");
}

} // namespace

int main() {
    try {
        allTransitionTypesReachDestination();
        easingIsBounded();
        engineInterruptsInsteadOfQueueing();
        randomTransitionsAvoidImmediateRepeats();
    } catch (const std::exception& error) {
        std::cerr << "transition_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "transition_test: all checks passed\n";
    return EXIT_SUCCESS;
}
