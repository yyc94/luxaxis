#pragma once

#include "luxaxis/result.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace luxaxis {

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> rgba;
};

using ImageDecoder = std::function<Result<DecodedImage>(const std::filesystem::path&)>;
using TextureHandle = std::shared_ptr<void>;

enum class ImageStatus : std::uint8_t {
    Missing,
    Loading,
    AwaitingUpload,
    Ready,
    Failed,
};

struct UploadRequest {
    std::filesystem::path path;
    std::uint64_t generation = 0;
    DecodedImage image;
};

struct ImageSnapshot {
    ImageStatus status = ImageStatus::Missing;
    TextureHandle texture;
    std::size_t textureBytes = 0;
    std::string error;
    std::uint64_t generation = 0;
    bool pinned = false;
};

class ImageCache {
  public:
    ImageCache(std::size_t budgetBytes, ImageDecoder decoder);
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    [[nodiscard]] bool request(const std::filesystem::path& path);
    [[nodiscard]] bool refresh(const std::filesystem::path& path);
    [[nodiscard]] std::vector<UploadRequest> takeUploads();
    [[nodiscard]] bool completeUpload(const UploadRequest& request, TextureHandle texture, std::size_t textureBytes);
    [[nodiscard]] bool failUpload(const UploadRequest& request, std::string message);

    [[nodiscard]] TextureHandle texture(const std::filesystem::path& path);
    [[nodiscard]] ImageSnapshot snapshot(const std::filesystem::path& path) const;
    void setPinned(const std::set<std::filesystem::path>& paths);
    void setBudget(std::size_t budgetBytes);

    [[nodiscard]] std::size_t residentBytes() const;
    [[nodiscard]] std::size_t budgetBytes() const;
    void waitForIdle();

  private:
    struct Entry {
        ImageStatus status = ImageStatus::Missing;
        TextureHandle texture;
        std::size_t textureBytes = 0;
        std::optional<DecodedImage> decoded;
        std::string error;
        std::uint64_t generation = 0;
        std::uint64_t lastUsed = 0;
        bool pinned = false;
    };

    struct DecodeJob {
        std::filesystem::path path;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] bool enqueue(const std::filesystem::path& path, bool force);
    void workerLoop();
    void evictLocked();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::map<std::filesystem::path, Entry> entries_;
    std::set<std::filesystem::path> pinnedPaths_;
    std::deque<DecodeJob> jobs_;
    ImageDecoder decoder_;
    std::thread worker_;
    std::size_t budgetBytes_ = 0;
    std::size_t residentBytes_ = 0;
    std::size_t activeJobs_ = 0;
    std::uint64_t clock_ = 0;
    bool stopping_ = false;
};

} // namespace luxaxis
