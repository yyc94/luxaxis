#pragma once

#include "luxaxis/config.hpp"
#include "luxaxis/engine.hpp"
#include "luxaxis/image_cache.hpp"

#include <src/plugins/PluginAPI.hpp>

#include <memory>

namespace luxaxis::hyprland {

class Adapter {
  public:
    Adapter(HANDLE handle, std::filesystem::path configPath, std::filesystem::path home);
    ~Adapter();

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    [[nodiscard]] SP<SHyprCtlCommand> registerReloadCommand();
    [[nodiscard]] SP<SHyprCtlCommand> registerSpotlightCommand();
    void unregisterCommands();

    [[nodiscard]] std::string reload();
    [[nodiscard]] std::string spotlight(std::string args);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace luxaxis::hyprland
