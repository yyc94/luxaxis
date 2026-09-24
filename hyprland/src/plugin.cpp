#include <src/plugins/PluginAPI.hpp>

#include "hyprland_adapter.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

constexpr auto PLUGIN_NAME = "luxaxis";
std::unique_ptr<luxaxis::hyprland::Adapter> adapter;

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
    const auto xdg = std::getenv("XDG_CONFIG_HOME");
    const auto home = std::getenv("HOME");
    const auto path = luxaxis::configPath(xdg ? xdg : "", home ? home : "");
    if (!path)
        throw std::runtime_error(path.error().message);
    const std::filesystem::path homePath = home ? home : "";
    adapter = std::make_unique<luxaxis::hyprland::Adapter>(handle, path.value(), homePath);
    (void)adapter->registerReloadCommand();
    (void)adapter->registerSpotlightCommand();
    return {PLUGIN_NAME, "Workspace-aware wallpaper and Spotlight renderer", "Luxaxis contributors", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (adapter)
        adapter->unregisterCommands();
    adapter.reset();
}
