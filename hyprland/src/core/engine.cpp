#include "luxaxis/engine.hpp"

#include <algorithm>
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
    outputs_.insert_or_assign(output.name, std::move(output));
    changed();
    return true;
}

bool Engine::removeOutput(const std::string& name) {
    if (outputs_.erase(name) == 0)
        return false;
    if (focusedOutput_ == name)
        focusedOutput_.reset();
    changed();
    return true;
}

bool Engine::activateWorkspace(const std::string& output, const std::int64_t workspace) {
    if (workspace <= 0)
        return false;
    const auto found = outputs_.find(output);
    if (found == outputs_.end() || found->second.workspace == workspace)
        return false;
    found->second.workspace = workspace;
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
        .wallpaperCandidates = {profile.wallpaper},
        .fallbackColor = config_.fallbackColor,
        .cursorLocal = {cursor_.x - output->second.logicalBounds.position.x, cursor_.y - output->second.logicalBounds.position.y},
        .maskEnabled = hasSpotlight,
        .revealEnabled = hasSpotlight && selectedOutput == outputName,
        .revision = revision_,
    };

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

} // namespace luxaxis
