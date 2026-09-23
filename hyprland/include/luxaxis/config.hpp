#pragma once

#include "luxaxis/result.hpp"
#include "luxaxis/types.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace luxaxis {

[[nodiscard]] Result<std::filesystem::path> configPath(
    std::string_view xdgConfigHome,
    std::string_view home);

[[nodiscard]] Result<Config> parseConfig(
    std::string_view text,
    const std::filesystem::path& source,
    const std::filesystem::path& home);

[[nodiscard]] Result<Config> loadConfig(
    const std::filesystem::path& source,
    const std::filesystem::path& home);

class ActiveConfig {
  public:
    [[nodiscard]] bool tryReplace(
        std::string_view text,
        const std::filesystem::path& source,
        const std::filesystem::path& home);

    [[nodiscard]] std::shared_ptr<const Config> current() const;
    [[nodiscard]] const std::optional<Error>& lastError() const;

  private:
    std::shared_ptr<const Config> current_;
    std::optional<Error> lastError_;
};

} // namespace luxaxis
