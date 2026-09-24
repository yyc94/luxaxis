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
    std::map<std::string, Profile> oldProfiles;
    for (const auto& [output, state] : outputs_)
        oldProfiles.emplace(output, resolveProfile(state.workspace).second);
    config_ = std::move(config);
    const Transition reloadTransition{
        .type = TransitionType::Fade,
        .durationMs = 120,
        .easing = "ease-out",
        .origin = TransitionOrigin::Cursor,
        .point = {0.5, 0.5},
        .randomAllowlist = {},
    };
    for (const auto& [output, state] : outputs_) {
        const auto destination = resolveProfile(state.workspace).second;
        const auto previous = oldProfiles.find(output);
        if (previous != oldProfiles.end() && previous->second != destination) {
            transitions_[output] = TransitionState{
                .previousProfile = previous->second,
                .transition = reloadTransition,
                .elapsed = {},
                .interruptedSourceProfile = std::nullopt,
                .interruptedTransition = {},
                .interruptedProgress = 0.0,
                .interruptedOrigin = {},
                .active = true,
            };
        } else {
            transitions_.erase(output);
        }
    }
    changed();
}

bool Engine::upsertOutput(OutputState output) {
    if (output.name.empty() || output.logicalBounds.size.x <= 0.0 || output.logicalBounds.size.y <= 0.0)
        return false;
    if (output.workspace && *output.workspace <= 0)
        output.workspace.reset();

    const auto existing = outputs_.find(output.name);
    if (existing != outputs_.end() && !output.workspace)
        output.workspace = existing->second.workspace;
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
    const auto cursorLocal = Vec2{cursor_.x - output->second.logicalBounds.position.x, cursor_.y - output->second.logicalBounds.position.y};
    const auto configuredTransition = profile.transition.value_or(config_.transition.value_or(Transition{}));
    std::optional<Profile> previousProfile;
    std::optional<Profile> interruptedSourceProfile;
    auto activeTransition = configuredTransition;
    double transitionProgress = 1.0;
    auto interruptedTransition = Transition{};
    double interruptedProgress = 0.0;
    auto interruptedOrigin = Vec2{};
    bool transitioning = false;
    if (const auto state = transitions_.find(outputName); state != transitions_.end() && state->second.active) {
        previousProfile = state->second.previousProfile;
        interruptedSourceProfile = state->second.interruptedSourceProfile;
        activeTransition = state->second.transition;
        transitionProgress = std::clamp(
            static_cast<double>(state->second.elapsed.count()) / static_cast<double>(state->second.transition.durationMs), 0.0, 1.0);
        interruptedTransition = state->second.interruptedTransition;
        interruptedProgress = state->second.interruptedProgress;
        interruptedOrigin = state->second.interruptedOrigin;
        transitioning = true;
    }
    const bool hasSpotlight = spotlightEnabled_ &&
        (profile.spotlight.type != SpotlightType::None || (transitioning && previousProfile && previousProfile->spotlight.type != SpotlightType::None));

    const auto& defaultWallpaper = config_.profiles.at(config_.defaultProfile).wallpaper;
    std::vector<std::filesystem::path> wallpaperCandidates{profile.wallpaper};
    if (defaultWallpaper != profile.wallpaper)
        wallpaperCandidates.push_back(defaultWallpaper);
    const auto addCandidate = [&wallpaperCandidates](const std::filesystem::path& path) {
        if (std::find(wallpaperCandidates.begin(), wallpaperCandidates.end(), path) == wallpaperCandidates.end())
            wallpaperCandidates.push_back(path);
    };
    if (previousProfile)
        addCandidate(previousProfile->wallpaper);
    if (interruptedSourceProfile)
        addCandidate(interruptedSourceProfile->wallpaper);
    const auto transitionOriginPoint = transitionOrigin(activeTransition, output->second);
    if (!spotlightEnabled_)
        return RenderPlan{
            .output = outputName,
            .logicalBounds = output->second.logicalBounds,
            .workspace = output->second.workspace,
            .profileName = profileName,
            .profile = Profile{profile.wallpaper, profile.fit, profile.position, {}, profile.transition},
            .previousProfile = previousProfile,
            .interruptedSourceProfile = interruptedSourceProfile,
            .wallpaperCandidates = std::move(wallpaperCandidates),
            .fallbackColor = config_.fallbackColor,
            .cursorLocal = cursorLocal,
            .maskEnabled = false,
            .revealEnabled = false,
            .transition = activeTransition,
            .transitionProgress = transitionProgress,
            .transitionOrigin = transitionOriginPoint,
            .interruptedTransition = interruptedTransition,
            .interruptedProgress = interruptedProgress,
            .interruptedOrigin = interruptedOrigin,
            .transitioning = transitioning,
            .revision = revision_,
        };
    return RenderPlan{
        .output = outputName,
        .logicalBounds = output->second.logicalBounds,
        .workspace = output->second.workspace,
        .profileName = profileName,
        .profile = profile,
        .previousProfile = previousProfile,
        .interruptedSourceProfile = interruptedSourceProfile,
        .wallpaperCandidates = std::move(wallpaperCandidates),
        .fallbackColor = config_.fallbackColor,
        .cursorLocal = cursorLocal,
        .maskEnabled = hasSpotlight,
        .revealEnabled = hasSpotlight && selectedOutput == outputName,
        .transition = activeTransition,
        .transitionProgress = transitionProgress,
        .transitionOrigin = transitionOriginPoint,
        .interruptedTransition = interruptedTransition,
        .interruptedProgress = interruptedProgress,
        .interruptedOrigin = interruptedOrigin,
        .transitioning = transitioning,
        .revision = revision_,
    };
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

Vec2 Engine::transitionOrigin(const Transition& transition, const OutputState& output) const {
    if (transition.origin == TransitionOrigin::Center)
        return {output.logicalBounds.size.x * 0.5, output.logicalBounds.size.y * 0.5};
    if (transition.origin == TransitionOrigin::Point)
        return {output.logicalBounds.size.x * transition.point.x, output.logicalBounds.size.y * transition.point.y};
    return {cursor_.x - output.logicalBounds.position.x, cursor_.y - output.logicalBounds.position.y};
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
            if (choices.empty()) {
                transition.type = TransitionType::Fade;
            } else {
                std::uniform_int_distribution<std::size_t> distribution(0, choices.size() - 1);
                transition.type = choices[distribution(generator)];
                lastRandomTransitions_[output] = transition.type;
            }
        }
    }
    if (transition.type == TransitionType::None) {
        transitions_.erase(output);
        return;
    }

    const auto source = resolveProfile(oldWorkspace).second;
    TransitionState next{
        .previousProfile = source,
        .transition = transition,
        .elapsed = {},
        .interruptedSourceProfile = std::nullopt,
        .interruptedTransition = {},
        .interruptedProgress = 0.0,
        .interruptedOrigin = {},
        .active = true,
    };
    if (const auto previous = transitions_.find(output); previous != transitions_.end() && previous->second.active) {
        next.interruptedSourceProfile = previous->second.previousProfile;
        next.interruptedTransition = previous->second.transition;
        next.interruptedProgress = std::clamp(
            static_cast<double>(previous->second.elapsed.count()) / static_cast<double>(previous->second.transition.durationMs), 0.0, 1.0);
        next.interruptedOrigin = transitionOrigin(previous->second.transition, outputs_.at(output));
    }
    transitions_[output] = std::move(next);
}

} // namespace luxaxis
