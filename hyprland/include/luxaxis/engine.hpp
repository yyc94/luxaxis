#pragma once

#include "luxaxis/types.hpp"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace luxaxis {

struct OutputState {
    std::string name;
    Rect logicalBounds;
    std::optional<std::int64_t> workspace;

    friend bool operator==(const OutputState&, const OutputState&) = default;
};

struct RenderPlan {
    std::string output;
    Rect logicalBounds;
    std::optional<std::int64_t> workspace;
    std::string profileName;
    Profile profile;
    std::optional<Profile> previousProfile;
    std::vector<std::filesystem::path> wallpaperCandidates;
    Color fallbackColor;
    Vec2 cursorLocal;
    bool maskEnabled = false;
    bool revealEnabled = false;
    Transition transition{};
    double transitionProgress = 1.0;
    Vec2 transitionOrigin{};
    bool transitioning = false;
    std::uint64_t revision = 0;

    friend bool operator==(const RenderPlan&, const RenderPlan&) = default;
};

class Engine {
  public:
    explicit Engine(Config config);

    void applyConfig(Config config);
    [[nodiscard]] bool upsertOutput(OutputState output);
    [[nodiscard]] bool removeOutput(const std::string& name);
    [[nodiscard]] bool activateWorkspace(const std::string& output, std::int64_t workspace);
    [[nodiscard]] bool setOutputBounds(const std::string& output, Rect logicalBounds);
    [[nodiscard]] bool setCursor(Vec2 globalPosition);
    [[nodiscard]] bool setFocusedOutput(std::optional<std::string> output);
    void advance(std::chrono::milliseconds elapsed);

    void spotlightOn();
    void spotlightOff();
    void spotlightToggle();
    [[nodiscard]] bool spotlightEnabled() const;

    [[nodiscard]] std::optional<RenderPlan> planFor(const std::string& output) const;
    [[nodiscard]] std::vector<RenderPlan> plans() const;
    [[nodiscard]] std::optional<std::string> activeOutput() const;
    [[nodiscard]] std::uint64_t revision() const;

  private:
    [[nodiscard]] std::pair<std::string, const Profile&> resolveProfile(std::optional<std::int64_t> workspace) const;
    void startTransition(const std::string& output, std::optional<std::int64_t> oldWorkspace, std::optional<std::int64_t> newWorkspace);
    void changed();

    Config config_;
    std::map<std::string, OutputState> outputs_;
    Vec2 cursor_{};
    std::optional<std::string> focusedOutput_;
    bool spotlightEnabled_ = true;
    std::uint64_t revision_ = 1;

    struct TransitionState {
        Profile previousProfile;
        Transition transition;
        std::chrono::milliseconds elapsed{};
        bool active = false;
    };
    std::map<std::string, TransitionState> transitions_;
    std::map<std::string, TransitionType> lastRandomTransitions_;
};

} // namespace luxaxis
