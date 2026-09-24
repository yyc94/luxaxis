#include "luxaxis/config.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::string_view VALID_CONFIG = R"(
version = 1
default_profile = "default"
active_output = "cursor"
excluded_outputs = ["projector"]
texture_cache_mib = 128
fallback_color = "#102030"

[profiles.default]
wallpaper = "~/Pictures/base.webp"

[profiles.focus]
wallpaper = "/wallpapers/focus.png"
fit = "contain"
position = [0.25, 0.75]

[profiles.focus.spotlight]
type = "circle"
mask_color = "#000000"
mask_opacity = 0.55
radius = "18%"
softness = "24px"

[profiles.beam]
wallpaper = "/wallpapers/beam.jpg"

[profiles.beam.spotlight]
type = "fan"
mask_color = "#102b12"
mask_opacity = 0.68
anchor = [0.5, 0.08]
radius = "18%"
aspect_ratio = 0.55
softness = "4%"
beam_start_reveal = 0.35

[profiles.strip]
wallpaper = "/wallpapers/strip.jpeg"

[profiles.strip.spotlight]
type = "strip"
mask_color = "#000000"
mask_opacity = 0.4
orientation = "vertical"
thickness = "200px"
softness = "3%"

[workspaces]
"1" = "default"
"2" = "focus"
"3" = "beam"
"4" = "strip"
)";

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void validConfigParses() {
    const auto parsed = luxaxis::parseConfig(VALID_CONFIG, "/config/luxaxis.toml", "/home/tester");
    require(parsed.hasValue(), parsed.hasValue() ? "" : parsed.error().message);
    const auto& config = parsed.value();
    require(config.textureCacheBytes == 128ULL * 1024ULL * 1024ULL, "cache size was not converted to bytes");
    require(config.profiles.at("default").wallpaper == "/home/tester/Pictures/base.webp", "home path was not expanded");
    require(config.profiles.at("focus").fit == luxaxis::FitMode::Contain, "fit mode did not parse");
    require(config.profiles.at("focus").spotlight.radius.resolve(1000.0) == 180.0, "percentage did not resolve against the short edge");
    require(config.profiles.at("beam").spotlight.type == luxaxis::SpotlightType::Fan, "fan did not parse");
    require(config.profiles.at("strip").spotlight.orientation == luxaxis::StripOrientation::Vertical, "strip orientation did not parse");
    require(config.workspaces.at(3) == "beam", "workspace mapping did not parse");
}

void strictValidationRejectsInvalidCandidates() {
    auto unknown = std::string{VALID_CONFIG};
    unknown.insert(unknown.find("[profiles.default]"), "unknown = true\n\n");
    auto parsed = luxaxis::parseConfig(unknown, "/config/luxaxis.toml", "/home/tester");
    require(!parsed, "unknown root field was accepted");
    require(parsed.error().message.contains("unknown field"), "unknown-field diagnostic lacked context");

    auto missingProfile = std::string{VALID_CONFIG};
    missingProfile.replace(missingProfile.find("\"2\" = \"focus\""), 13, "\"2\" = \"missing\"");
    parsed = luxaxis::parseConfig(missingProfile, "/config/luxaxis.toml", "/home/tester");
    require(!parsed && parsed.error().message.contains("missing profile"), "missing profile reference was accepted");

    auto specialWorkspace = std::string{VALID_CONFIG};
    specialWorkspace.replace(specialWorkspace.find("\"4\" = \"strip\""), 13, "\"special:x\" = \"strip\"");
    parsed = luxaxis::parseConfig(specialWorkspace, "/config/luxaxis.toml", "/home/tester");
    require(!parsed && parsed.error().message.contains("positive integer"), "special workspace was accepted");

    auto badExtension = std::string{VALID_CONFIG};
    badExtension.replace(badExtension.find("base.webp"), 9, "base.gif");
    parsed = luxaxis::parseConfig(badExtension, "/config/luxaxis.toml", "/home/tester");
    require(!parsed && parsed.error().message.contains("PNG, JPEG, or WebP"), "unsupported image was accepted");

    auto unknownTransitionField = std::string{VALID_CONFIG};
    unknownTransitionField.insert(unknownTransitionField.find("[profiles.default]"), "[transition]\ntype = \"fade\"\npoint = [0.2, 0.3]\n\n");
    parsed = luxaxis::parseConfig(unknownTransitionField, "/config/luxaxis.toml", "/home/tester");
    require(!parsed && parsed.error().message.contains("unknown field 'point'"), "unknown transition field was accepted");

    auto duplicateWorkspace = std::string{VALID_CONFIG};
    duplicateWorkspace.insert(duplicateWorkspace.find("\"2\" = \"focus\""), "\"01\" = \"default\"\n");
    parsed = luxaxis::parseConfig(duplicateWorkspace, "/config/luxaxis.toml", "/home/tester");
    require(!parsed && parsed.error().message.contains("duplicate workspace ID"), "normalized duplicate workspace was accepted");
}

void invalidReplacementPreservesActiveConfig() {
    luxaxis::ActiveConfig active;
    require(active.tryReplace(VALID_CONFIG, "/config/luxaxis.toml", "/home/tester"), "valid candidate was rejected");
    const auto original = active.current();
    require(original != nullptr, "valid candidate did not become active");

    require(!active.tryReplace("version = 1", "/config/luxaxis.toml", "/home/tester"), "invalid candidate became active");
    require(active.current() == original, "invalid candidate replaced active config");
    require(active.lastError().has_value(), "invalid candidate did not retain an error");
}

void phaseTwoBTransitionConfigParsesStrictly() {
    auto text = std::string{VALID_CONFIG};
    text.insert(text.find("[profiles.default]"), "[transition]\ntype = \"fade\"\nduration_ms = 180\neasing = \"ease-out\"\norigin = \"center\"\n\n");
    text.insert(text.find("[profiles.focus]"), "[profiles.default.transition]\ntype = \"random\"\nallowlist = [\"wipe\", \"grow\"]\n\n");
    auto parsed = luxaxis::parseConfig(text, "/config/luxaxis.toml", "/home/tester");
    require(parsed.hasValue(), "transition configuration was rejected");
    require(parsed.value().transition && parsed.value().transition->durationMs == 180, "global transition did not parse");
    require(parsed.value().profiles.at("default").transition && parsed.value().profiles.at("default").transition->randomAllowlist.size() == 2,
            "random transition allowlist did not parse");

    text.insert(text.find("[profiles.beam]"), "[profiles.beam.transition]\ntype = \"random\"\nallowlist = [\"wipe\", \"wipe\"]\n\n");
    parsed = luxaxis::parseConfig(text, "/config/luxaxis.toml", "/home/tester");
    require(!parsed && parsed.error().message.contains("unique"), "duplicate random transition was accepted");
}

void configPathFollowsXdgRules() {
    auto path = luxaxis::configPath("/xdg", "/home/tester");
    require(path && path.value() == "/xdg/hypr/luxaxis.toml", "XDG_CONFIG_HOME was not preferred");
    path = luxaxis::configPath("", "/home/tester");
    require(path && path.value() == "/home/tester/.config/hypr/luxaxis.toml", "HOME fallback was incorrect");
    path = luxaxis::configPath("relative", "/home/tester");
    require(!path, "relative XDG_CONFIG_HOME was accepted");
}

} // namespace

int main() {
    try {
        validConfigParses();
        strictValidationRejectsInvalidCandidates();
        invalidReplacementPreservesActiveConfig();
        configPathFollowsXdgRules();
        phaseTwoBTransitionConfigParsesStrictly();
    } catch (const std::exception& error) {
        std::cerr << "config_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "config_test: all checks passed\n";
    return EXIT_SUCCESS;
}
