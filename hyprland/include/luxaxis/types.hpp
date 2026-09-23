#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace luxaxis {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Vec2&, const Vec2&) = default;
};

struct Rect {
    Vec2 position{};
    Vec2 size{};

    [[nodiscard]] bool contains(Vec2 point) const;
    friend bool operator==(const Rect&, const Rect&) = default;
};

struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    friend bool operator==(const Color&, const Color&) = default;
};

enum class LengthUnit : std::uint8_t {
    Pixels,
    ShortEdgePercent,
};

struct Length {
    double value = 0.0;
    LengthUnit unit = LengthUnit::Pixels;

    [[nodiscard]] double resolve(double shortEdge) const;
    friend bool operator==(const Length&, const Length&) = default;
};

enum class FitMode : std::uint8_t {
    Cover,
    Contain,
    Stretch,
};

enum class ActiveOutputPolicy : std::uint8_t {
    Cursor,
    Focused,
};

enum class SpotlightType : std::uint8_t {
    None,
    Circle,
    Strip,
    Fan,
};

enum class StripOrientation : std::uint8_t {
    Horizontal,
    Vertical,
};

struct Spotlight {
    SpotlightType type = SpotlightType::None;
    Color maskColor{};
    double maskOpacity = 0.0;
    Length radius{};
    Length softness{};
    StripOrientation orientation = StripOrientation::Horizontal;
    Length thickness{};
    Vec2 anchor{0.5, 0.0};
    double aspectRatio = 1.0;
    double beamStartReveal = 1.0;

    friend bool operator==(const Spotlight&, const Spotlight&) = default;
};

struct Profile {
    std::filesystem::path wallpaper;
    FitMode fit = FitMode::Cover;
    Vec2 position{0.5, 0.5};
    Spotlight spotlight{};

    friend bool operator==(const Profile&, const Profile&) = default;
};

struct Config {
    std::uint32_t version = 1;
    std::string defaultProfile;
    ActiveOutputPolicy activeOutput = ActiveOutputPolicy::Cursor;
    std::set<std::string> excludedOutputs;
    std::size_t textureCacheBytes = 256ULL * 1024ULL * 1024ULL;
    Color fallbackColor{};
    std::map<std::string, Profile> profiles;
    std::map<std::int64_t, std::string> workspaces;

    friend bool operator==(const Config&, const Config&) = default;
};

inline double Length::resolve(const double shortEdge) const {
    return unit == LengthUnit::Pixels ? value : shortEdge * value / 100.0;
}

inline bool Rect::contains(const Vec2 point) const {
    return size.x > 0.0 && size.y > 0.0 && point.x >= position.x && point.y >= position.y && point.x < position.x + size.x && point.y < position.y + size.y;
}

} // namespace luxaxis
