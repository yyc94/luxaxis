#include "luxaxis/config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace luxaxis {
namespace {

using KeySet = std::unordered_set<std::string_view>;

class ValidationError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& context, const std::string& message) {
    throw ValidationError(context + ": " + message);
}

void ensureOnly(const toml::table& table, const KeySet& allowed, const std::string& context) {
    for (const auto& [key, unused] : table) {
        (void)unused;
        if (!allowed.contains(key.str()))
            fail(context, "unknown field '" + std::string{key.str()} + "'");
    }
}

const toml::node& requireNode(const toml::table& table, const std::string_view key, const std::string& context) {
    const auto* node = table.get(key);
    if (!node)
        fail(context, "missing required field '" + std::string{key} + "'");
    return *node;
}

const toml::table& requireTable(const toml::node& node, const std::string& context) {
    const auto* table = node.as_table();
    if (!table)
        fail(context, "expected a table");
    return *table;
}

std::string requireString(const toml::table& table, const std::string_view key, const std::string& context) {
    const auto value = requireNode(table, key, context).value<std::string>();
    if (!value)
        fail(context + "." + std::string{key}, "expected a string");
    return *value;
}

double number(const toml::node& node, const std::string& context) {
    const auto value = node.value<double>();
    if (!value || !std::isfinite(*value))
        fail(context, "expected a finite number");
    return *value;
}

double requireNumber(const toml::table& table, const std::string_view key, const std::string& context) {
    return number(requireNode(table, key, context), context + "." + std::string{key});
}

double bounded(const double value, const double low, const double high, const std::string& context) {
    if (value < low || value > high)
        fail(context, "expected a value in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
    return value;
}

Vec2 vec2(const toml::node& node, const std::string& context) {
    const auto* array = node.as_array();
    if (!array || array->size() != 2)
        fail(context, "expected an array of two numbers");

    return {
        bounded(number(*array->get(0), context + "[0]"), 0.0, 1.0, context + "[0]"),
        bounded(number(*array->get(1), context + "[1]"), 0.0, 1.0, context + "[1]"),
    };
}

Color color(const std::string& value, const std::string& context) {
    if (value.size() != 7 || value.front() != '#')
        fail(context, "expected #RRGGBB");

    std::array<unsigned int, 3> channels{};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const auto begin = value.data() + 1 + index * 2;
        const auto end = begin + 2;
        const auto [position, error] = std::from_chars(begin, end, channels[index], 16);
        if (error != std::errc{} || position != end)
            fail(context, "expected #RRGGBB");
    }

    return {
        channels[0] / 255.0,
        channels[1] / 255.0,
        channels[2] / 255.0,
    };
}

Length length(const std::string& value, const std::string& context, const bool allowZero) {
    LengthUnit unit;
    std::string_view numeric;
    double maximum = std::numeric_limits<double>::max();

    if (value.ends_with("px")) {
        unit = LengthUnit::Pixels;
        numeric = std::string_view{value}.substr(0, value.size() - 2);
    } else if (value.ends_with('%')) {
        unit = LengthUnit::ShortEdgePercent;
        numeric = std::string_view{value}.substr(0, value.size() - 1);
        maximum = 100.0;
    } else {
        fail(context, "expected a length ending in 'px' or '%'");
    }

    double parsed = 0.0;
    const auto [position, error] = std::from_chars(numeric.data(), numeric.data() + numeric.size(), parsed);
    if (error != std::errc{} || position != numeric.data() + numeric.size() || !std::isfinite(parsed))
        fail(context, "invalid length '" + value + "'");
    if (parsed < 0.0 || (!allowZero && parsed == 0.0) || parsed > maximum)
        fail(context, "length is outside its accepted range");
    return {parsed, unit};
}

std::filesystem::path wallpaperPath(const std::string& raw, const std::filesystem::path& home, const std::string& context) {
    std::filesystem::path result;
    if (raw.starts_with("~/")) {
        if (home.empty() || !home.is_absolute())
            fail(context, "cannot expand '~' without an absolute home path");
        result = home / raw.substr(2);
    } else {
        result = raw;
    }

    if (!result.is_absolute())
        fail(context, "wallpaper path must be absolute or start with '~/'");

    auto extension = result.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".webp")
        fail(context, "wallpaper must be a PNG, JPEG, or WebP image");
    return result.lexically_normal();
}

FitMode fitMode(const std::string& value, const std::string& context) {
    if (value == "cover")
        return FitMode::Cover;
    if (value == "contain")
        return FitMode::Contain;
    if (value == "stretch")
        return FitMode::Stretch;
    fail(context, "expected 'cover', 'contain', or 'stretch'");
}

Spotlight spotlight(const toml::table& table, const std::string& context) {
    const auto type = requireString(table, "type", context);
    Spotlight result;

    if (type == "none") {
        ensureOnly(table, {"type"}, context);
        return result;
    }

    result.maskColor = color(requireString(table, "mask_color", context), context + ".mask_color");
    result.maskOpacity = bounded(requireNumber(table, "mask_opacity", context), 0.0, 1.0, context + ".mask_opacity");
    result.softness = length(requireString(table, "softness", context), context + ".softness", true);

    if (type == "circle") {
        ensureOnly(table, {"type", "mask_color", "mask_opacity", "radius", "softness"}, context);
        result.type = SpotlightType::Circle;
        result.radius = length(requireString(table, "radius", context), context + ".radius", false);
        return result;
    }

    if (type == "strip") {
        ensureOnly(table, {"type", "mask_color", "mask_opacity", "orientation", "thickness", "softness"}, context);
        result.type = SpotlightType::Strip;
        result.thickness = length(requireString(table, "thickness", context), context + ".thickness", false);
        const auto orientation = requireString(table, "orientation", context);
        if (orientation == "horizontal")
            result.orientation = StripOrientation::Horizontal;
        else if (orientation == "vertical")
            result.orientation = StripOrientation::Vertical;
        else
            fail(context + ".orientation", "expected 'horizontal' or 'vertical'");
        return result;
    }

    if (type == "fan") {
        ensureOnly(
            table,
            {"type", "mask_color", "mask_opacity", "anchor", "radius", "aspect_ratio", "softness", "beam_start_reveal"},
            context);
        result.type = SpotlightType::Fan;
        result.anchor = vec2(requireNode(table, "anchor", context), context + ".anchor");
        result.radius = length(requireString(table, "radius", context), context + ".radius", false);
        result.aspectRatio = bounded(requireNumber(table, "aspect_ratio", context), 0.05, 10.0, context + ".aspect_ratio");
        result.beamStartReveal = bounded(requireNumber(table, "beam_start_reveal", context), 0.0, 1.0, context + ".beam_start_reveal");
        return result;
    }

    fail(context + ".type", "expected 'none', 'circle', 'strip', or 'fan'");
}

TransitionType transitionType(const std::string& value, const std::string& context) {
    if (value == "none") return TransitionType::None;
    if (value == "fade") return TransitionType::Fade;
    if (value == "wipe") return TransitionType::Wipe;
    if (value == "grow") return TransitionType::Grow;
    if (value == "outer") return TransitionType::Outer;
    if (value == "clock") return TransitionType::Clock;
    if (value == "random") return TransitionType::Random;
    fail(context, "expected none, fade, wipe, grow, outer, clock, or random");
}

Transition transition(const toml::table& table, const std::string& context) {
    ensureOnly(table, {"type", "duration_ms", "easing", "origin", "allowlist"}, context);
    Transition result;
    result.type = transitionType(requireString(table, "type", context), context + ".type");

    if (const auto duration = table["duration_ms"].value<std::int64_t>()) {
        if (*duration < 50 || *duration > 2000)
            fail(context + ".duration_ms", "expected an integer in [50, 2000]");
        result.durationMs = static_cast<std::uint32_t>(*duration);
    } else if (table.contains("duration_ms")) {
        fail(context + ".duration_ms", "expected an integer");
    }

    if (const auto easing = table["easing"].value<std::string>()) {
        if (*easing != "linear" && *easing != "ease-in" && *easing != "ease-out" && *easing != "ease-in-out")
            fail(context + ".easing", "expected linear, ease-in, ease-out, or ease-in-out");
        result.easing = *easing;
    } else if (table.contains("easing")) {
        fail(context + ".easing", "expected a string");
    }

    if (const auto* origin = table.get("origin")) {
        if (const auto value = origin->value<std::string>()) {
            if (*value == "cursor") result.origin = TransitionOrigin::Cursor;
            else if (*value == "center") result.origin = TransitionOrigin::Center;
            else fail(context + ".origin", "expected cursor, center, or a normalized point");
        } else {
            result.origin = TransitionOrigin::Point;
            result.point = vec2(*origin, context + ".origin");
        }
    }
    if (result.type == TransitionType::Random) {
        const auto* allowlist = table.get("allowlist");
        if (!allowlist || !allowlist->is_array())
            fail(context + ".allowlist", "random transitions require an array");
        for (std::size_t index = 0; index < allowlist->as_array()->size(); ++index) {
            const auto value = allowlist->as_array()->get(index)->value<std::string>();
            if (!value)
                fail(context + ".allowlist[" + std::to_string(index) + "]", "expected a transition name");
            const auto type = transitionType(*value, context + ".allowlist[" + std::to_string(index) + "]");
            if (type == TransitionType::None || type == TransitionType::Random)
                fail(context + ".allowlist", "allowlist entries cannot be none or random");
            if (std::find(result.randomAllowlist.begin(), result.randomAllowlist.end(), type) != result.randomAllowlist.end())
                fail(context + ".allowlist", "allowlist entries must be unique");
            result.randomAllowlist.push_back(type);
        }
        if (result.randomAllowlist.empty())
            fail(context + ".allowlist", "allowlist cannot be empty");
    } else if (table.contains("allowlist")) {
        fail(context + ".allowlist", "allowlist is only valid for random transitions");
    }
    return result;
}

Profile profile(const toml::table& table, const std::filesystem::path& home, const std::string& context) {
    ensureOnly(table, {"wallpaper", "fit", "position", "spotlight", "transition"}, context);
    Profile result;
    result.wallpaper = wallpaperPath(requireString(table, "wallpaper", context), home, context + ".wallpaper");

    if (const auto value = table["fit"].value<std::string>())
        result.fit = fitMode(*value, context + ".fit");
    else if (table.contains("fit"))
        fail(context + ".fit", "expected a string");

    if (const auto* position = table.get("position"))
        result.position = vec2(*position, context + ".position");
    if (const auto* spotlightNode = table.get("spotlight"))
        result.spotlight = spotlight(requireTable(*spotlightNode, context + ".spotlight"), context + ".spotlight");
    if (const auto* transitionNode = table.get("transition"))
        result.transition = transition(requireTable(*transitionNode, context + ".transition"), context + ".transition");
    return result;
}

Config validate(const toml::table& root, const std::filesystem::path& home) {
    ensureOnly(
        root,
        {"version", "default_profile", "active_output", "excluded_outputs", "texture_cache_mib", "fallback_color", "transition", "profiles", "workspaces"},
        "root");

    Config result;
    const std::string rootContext = "root";
    const auto version = requireNode(root, "version", rootContext).value<std::int64_t>();
    if (!version || *version != 1)
        fail("root.version", "only version 1 is supported");
    result.version = 1;
    result.defaultProfile = requireString(root, "default_profile", rootContext);
    if (result.defaultProfile.empty())
        fail("root.default_profile", "profile name cannot be empty");

    if (const auto active = root["active_output"].value<std::string>()) {
        if (*active == "cursor")
            result.activeOutput = ActiveOutputPolicy::Cursor;
        else if (*active == "focused")
            result.activeOutput = ActiveOutputPolicy::Focused;
        else
            fail("root.active_output", "expected 'cursor' or 'focused'");
    } else if (root.contains("active_output")) {
        fail("root.active_output", "expected a string");
    }

    if (const auto* excludedNode = root.get("excluded_outputs")) {
        const auto* excluded = excludedNode->as_array();
        if (!excluded)
            fail("root.excluded_outputs", "expected an array of strings");
        for (std::size_t index = 0; index < excluded->size(); ++index) {
            const auto output = excluded->get(index)->value<std::string>();
            if (!output || output->empty())
                fail("root.excluded_outputs[" + std::to_string(index) + "]", "expected a non-empty string");
            if (!result.excludedOutputs.insert(*output).second)
                fail("root.excluded_outputs", "duplicate output '" + *output + "'");
        }
    }

    if (const auto cache = root["texture_cache_mib"].value<std::int64_t>()) {
        if (*cache < 1 || *cache > 65536)
            fail("root.texture_cache_mib", "expected an integer in [1, 65536]");
        result.textureCacheBytes = static_cast<std::size_t>(*cache) * 1024ULL * 1024ULL;
    } else if (root.contains("texture_cache_mib")) {
        fail("root.texture_cache_mib", "expected an integer");
    }

    if (const auto fallback = root["fallback_color"].value<std::string>())
        result.fallbackColor = color(*fallback, "root.fallback_color");
    else if (root.contains("fallback_color"))
        fail("root.fallback_color", "expected a string");

    if (const auto* transitionNode = root.get("transition"))
        result.transition = transition(requireTable(*transitionNode, "root.transition"), "root.transition");

    const std::string profilesContext = "root.profiles";
    const auto& profilesNode = requireNode(root, "profiles", rootContext);
    const auto& profiles = requireTable(profilesNode, profilesContext);
    if (profiles.empty())
        fail("root.profiles", "at least one profile is required");
    for (const auto& [name, node] : profiles) {
        if (name.str().empty())
            fail("root.profiles", "profile name cannot be empty");
        const auto context = "profiles." + std::string{name.str()};
        result.profiles.emplace(name.str(), profile(requireTable(node, context), home, context));
    }
    if (!result.profiles.contains(result.defaultProfile))
        fail("root.default_profile", "references missing profile '" + result.defaultProfile + "'");

    const std::string workspacesContext = "root.workspaces";
    const auto& workspacesNode = requireNode(root, "workspaces", rootContext);
    const auto& workspaces = requireTable(workspacesNode, workspacesContext);
    for (const auto& [workspace, node] : workspaces) {
        std::int64_t id = 0;
        const auto raw = workspace.str();
        const auto [position, error] = std::from_chars(raw.data(), raw.data() + raw.size(), id);
        if (error != std::errc{} || position != raw.data() + raw.size() || id <= 0)
            fail("workspaces." + std::string{raw}, "workspace ID must be a positive integer");
        const auto profileName = node.value<std::string>();
        if (!profileName)
            fail("workspaces." + std::string{raw}, "expected a profile name");
        if (!result.profiles.contains(*profileName))
            fail("workspaces." + std::string{raw}, "references missing profile '" + *profileName + "'");
        if (!result.workspaces.emplace(id, *profileName).second)
            fail("workspaces." + std::string{raw}, "duplicate workspace ID after numeric normalization");
    }
    return result;
}

} // namespace

Result<std::filesystem::path> configPath(const std::string_view xdgConfigHome, const std::string_view home) {
    std::filesystem::path base;
    if (!xdgConfigHome.empty())
        base = xdgConfigHome;
    else if (!home.empty())
        base = std::filesystem::path{home} / ".config";
    else
        return Error{"environment", "XDG_CONFIG_HOME and HOME are both unset"};

    if (!base.is_absolute())
        return Error{"environment", "configuration base path must be absolute"};
    return (base / "hypr" / "luxaxis.toml").lexically_normal();
}

Result<Config> parseConfig(const std::string_view text, const std::filesystem::path& source, const std::filesystem::path& home) {
    try {
        return validate(toml::parse(text, source.string()), home);
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << error.description() << " at line " << error.source().begin.line << ", column " << error.source().begin.column;
        return Error{source.string(), message.str()};
    } catch (const ValidationError& error) {
        return Error{source.string(), error.what()};
    }
}

Result<Config> loadConfig(const std::filesystem::path& source, const std::filesystem::path& home) {
    std::ifstream stream{source};
    if (!stream)
        return Error{source.string(), "unable to open configuration file"};
    std::ostringstream contents;
    contents << stream.rdbuf();
    if (!stream.good() && !stream.eof())
        return Error{source.string(), "failed while reading configuration file"};
    return parseConfig(contents.str(), source, home);
}

bool ActiveConfig::tryReplace(const std::string_view text, const std::filesystem::path& source, const std::filesystem::path& home) {
    auto candidate = parseConfig(text, source, home);
    if (!candidate) {
        lastError_ = candidate.error();
        return false;
    }

    current_ = std::make_shared<const Config>(std::move(candidate.value()));
    lastError_.reset();
    return true;
}

std::shared_ptr<const Config> ActiveConfig::current() const {
    return current_;
}

const std::optional<Error>& ActiveConfig::lastError() const {
    return lastError_;
}

} // namespace luxaxis
