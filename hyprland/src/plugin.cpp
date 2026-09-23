#include <src/plugins/PluginAPI.hpp>

#include <stdexcept>
#include <string>

namespace {

constexpr auto PLUGIN_NAME = "luxaxis";

void requireCompatibleHyprland(HANDLE handle) {
    const auto runtime = HyprlandAPI::getHyprlandVersion(handle);

    if (runtime.hash != GIT_COMMIT_HASH) {
        throw std::runtime_error(
            "Luxaxis was built for Hyprland commit " + std::string{GIT_COMMIT_HASH} +
            ", but the running compositor is " + runtime.hash);
    }
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    requireCompatibleHyprland(handle);
    return {PLUGIN_NAME, "Workspace-aware wallpaper and Spotlight renderer", "Luxaxis contributors", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {}
