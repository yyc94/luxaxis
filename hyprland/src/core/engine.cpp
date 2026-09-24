#include "luxaxis/engine.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <utility>

namespace luxaxis {

Engine::Engine(Config config) : config_(std::move(config)) {
    if (!config_.profiles.contains(config_.defaultProfile))
        throw std::invalid_argument("Engine requires a validated configuration");
}

void Engine::applyConfig(Config config) {
    if (!config.profiles.contains(config.defaultProfile))
        throw std::invalid_argument("Engine requires a validated configuration");
    config_ = std::move(config);
    changed();
}

bool Engine::upsertOutput(OutputState output) {
    if (output.name.empty() || output.logicalBounds.size.x <= 0.0 || output.logicalBounds.size.y <= 0.0)
        return false;
    if (output.workspace && *output.workspace <= 0)
        output.workspace.reset();

    const auto existing = outputs_.find(output.name);
    if (existing != outputs_.end() && existing->second == output)
        return false;
    const auto oldWorkspace = existing == outputs_.end() ? std::optional<std::int64_t>{} : existing->second.workspace;
    outputs_.insert_or_assign(output.name, std::move(output));
    if (existing != outputs_.end() && oldWorkspace != outputs_[existing->first].workspace)
        startTransition(existing->first, oldWorkspace, outputs_[existing->first].workspace);
    changed();
    return true;
}

bool Engine::removeOutput(const std::string& name) {
    if (outputs_.erase(name) == 0)
        return false;
    if (focusedOutput_ == name)
        focusedOutput_.reset();
    transitions_.erase(name);
    lastRandomTransitions_.erase(name);
    changed();
    return true;
}

bool Engine::activateWorkspace(const std::string& output, const std::int64_t workspace) {
    if (workspace <= 0)
        return false;
    const auto found = outputs_.find(output);
    if (found == outputs_.end() || found->second.workspace == workspace)
        return false;
    const auto oldWorkspace = found->second.workspace;
    found->second.workspace = workspace;
    startTransition(output, oldWorkspace, found->second.workspace);
    changed();
    return true;
}

bool Engine::setOutputBounds(const std::string& output, const Rect logicalBounds) {
    if (logicalBounds.size.x <= 0.0 || logicalBounds.size.y <= 0.0)
        return false;
    const auto found = outputs_.find(output);
    if (found == outputs_.end() || found->second.logicalBounds == logicalBounds)
        return false;
    found->second.logicalBounds = logicalBounds;
    changed();
    return true;
}

bool Engine::setCursor(const Vec2 globalPosition) {
    if (cursor_ == globalPosition)
        return false;
    cursor_ = globalPosition;
    changed();
    return true;
}

bool Engine::setFocusedOutput(std::optional<std::string> output) {
    if (focusedOutput_ == output)
        return false;
    focusedOutput_ = std::move(output);
    changed();
    return true;
}

void Engine::advance(const std::chrono::milliseconds elapsed) {
    if (elapsed.count() <= 0)
        return;
    bool changedState = false;
    for (auto& [output, state] : transitions_) {
        if (!state.active)
            continue;
        state.elapsed += elapsed;
        if (state.elapsed.count() >= state.transition.durationMs) {
            state.elapsed = std::chrono::milliseconds{state.transition.durationMs};
            state.active = false;
        }
        changedState = true;
    }
    if (changedState)
        changed();
}

void Engine::spotlightOn() {
    if (!spotlightEnabled_) {
        spotlightEnabled_ = true;
        changed();
    }
}

void Engine::spotlightOff() {
    if (spotlightEnabled_) {
        spotlightEnabled_ = false;
        changed();
    }
}

void Engine::spotlightToggle() {
    spotlightEnabled_ = !spotlightEnabled_;
    changed();
}

bool Engine::spotlightEnabled() const {
    return spotlightEnabled_;
}

std::pair<std::string, const Profile&> Engine::resolveProfile(const std::optional<std::int64_t> workspace) const {
    if (workspace) {
        if (const auto mapping = config_.workspaces.find(*workspace); mapping != config_.workspaces.end()) {
            if (const auto profile = config_.profiles.find(mapping->second); profile != config_.profiles.end())
                return {profile->first, profile->second};
        }
    }
    const auto& fallback = config_.profiles.at(config_.defaultProfile);
    return {config_.defaultProfile, fallback};
}

std::optional<std::string> Engine::activeOutput() const {
    if (config_.activeOutput == ActiveOutputPolicy::Focused) {
        if (focusedOutput_ && outputs_.contains(*focusedOutput_) && !config_.excludedOutputs.contains(*focusedOutput_))
            return focusedOutput_;
        return std::nullopt;
    }

    for (const auto& [name, output] : outputs_) {
        if (!config_.excludedOutputs.contains(name) && output.logicalBounds.contains(cursor_))
            return name;
    }
    return std::nullopt;
}

std::optional<RenderPlan> Engine::planFor(const std::string& outputName) const {
    const auto output = outputs_.find(outputName);
    if (output == outputs_.end() || config_.excludedOutputs.contains(outputName))
        return std::nullopt;

    const auto [profileName, profile] = resolveProfile(output->second.workspace);
    const auto selectedOutput = activeOutput();
    const bool hasSpotlight = spotlightEnabled_ && profile.spotlight.type != SpotlightType::None;

    RenderPlan plan{
        .output = outputName,
        .logicalBounds = output->second.logicalBounds,
        .workspace = output->second.workspace,
        .profileName = profileName,
        .profile = profile,
        .previousProfile = std::nullopt,
        .wallpaperCandidates = {profile.wallpaper},
        .fallbackColor = config_.fallbackColor,
        .cursorLocal = {cursor_.x - output->second.logicalBounds.position.x, cursor_.y - output->second.logicalBounds.position.y},
        .maskEnabled = hasSpotlight,
        .revealEnabled = hasSpotlight && selectedOutput == outputName,
        .transition = profile.transition.value_or(config_.transition.value_or(Transition{})),
        .transitionProgress = 1.0,
        .transitionOrigin = profile.transition.value_or(config_.transition.value_or(Transition{})).origin == TransitionOrigin::Center
            ? Vec2{output->second.logicalBounds.size.x * 0.5, output->second.logicalBounds.size.y * 0.5}
            : profile.transition.value_or(config_.transition.value_or(Transition{})).origin == TransitionOrigin::Point
            ? Vec2{output->second.logicalBounds.size.x * profile.transition.value_or(config_.transition.value_or(Transition{})).point.x,
                   output->second.logicalBounds.size.y * profile.transition.value_or(config_.transition.value_or(Transition{})).point.y}
            : Vec2{cursor_.x - output->second.logicalBounds.position.x, cursor_.y - output->second.logicalBounds.position.y},
        .transitioning = false,
        .revision = revision_,
    };

    if (const auto state = transitions_.find(outputName); state != transitions_.end() && state->second.active) {
        plan.previousProfile = state->second.previousProfile;
        plan.transition = state->second.transition;
        plan.transitionProgress = std::clamp(
            static_cast<double>(state->second.elapsed.count()) / static_cast<double>(state->second.transition.durationMs), 0.0, 1.0);
        plan.transitioning = true;
    }

    const auto& defaultWallpaper = config_.profiles.at(config_.defaultProfile).wallpaper;
    if (defaultWallpaper != profile.wallpaper)
        plan.wallpaperCandidates.push_back(defaultWallpaper);
    if (!spotlightEnabled_)
        plan.profile.spotlight = {};
    return plan;
}

std::vector<RenderPlan> Engine::plans() const {
    std::vector<RenderPlan> result;
    result.reserve(outputs_.size());
    for (const auto& [name, unused] : outputs_) {
        (void)unused;
        if (auto plan = planFor(name))
            result.push_back(std::move(*plan));
    }
    return result;
}

std::uint64_t Engine::revision() const {
    return revision_;
}

void Engine::changed() {
    ++revision_;
}

void Engine::startTransition(const std::string& output, const std::optional<std::int64_t> oldWorkspace, const std::optional<std::int64_t> newWorkspace) {
    const auto destination = resolveProfile(newWorkspace).second;
    auto transition = destination.transition.value_or(config_.transition.value_or(Transition{}));
    if (transition.type == TransitionType::Random) {
        static std::mt19937 generator{0x4C555841U};
        std::vector<TransitionType> choices = transition.randomAllowlist;
        if (choices.empty())
            transition.type = TransitionType::Fade;
        else {
            if (choices.size() > 1) {
                if (const auto previous = lastRandomTransitions_.find(output); previous != lastRandomTransitions_.end()) {
                    choices.erase(std::remove(choices.begin(), choices.end(), previous->second), choices.end());
                }
            }
            std::uniform_int_distribution<std::size_t> distribution(0, choices.size() - 1);
            transition.type = choices[distribution(generator)];
            lastRandomTransitions_[output] = transition.type;
        }
    }
    if (transition.type == TransitionType::None) {
        transitions_.erase(output);
        return;
    }

    const auto source = resolveProfile(oldWorkspace).second;
    transitions_[output] = TransitionState{
        .previousProfile = source,
        .transition = transition,
        .elapsed = {},
        .active = true,
    };
}

} // namespace luxaxis
