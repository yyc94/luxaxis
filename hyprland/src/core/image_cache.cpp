#include "luxaxis/image_cache.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace luxaxis {
namespace {

std::optional<std::string> validate(const DecodedImage& image) {
    if (image.width == 0 || image.height == 0)
        return "decoded image has zero dimensions";
    const auto minimumStride = static_cast<std::uint64_t>(image.width) * 4ULL;
    if (image.stride < minimumStride)
        return "decoded RGBA stride is too small";
    const auto minimumBytes = static_cast<std::uint64_t>(image.stride) * image.height;
    if (minimumBytes > std::numeric_limits<std::size_t>::max() || image.rgba.size() < minimumBytes)
        return "decoded RGBA buffer is truncated";
    return std::nullopt;
}

} // namespace

ImageCache::ImageCache(const std::size_t budgetBytes, ImageDecoder decoder, ImageReadyCallback readyCallback)
    : decoder_(std::move(decoder)), readyCallback_(std::move(readyCallback)), budgetBytes_(budgetBytes) {
    worker_ = std::thread([this] { workerLoop(); });
}

ImageCache::~ImageCache() {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

bool ImageCache::request(const std::filesystem::path& path) {
    return enqueue(path, false);
}

bool ImageCache::refresh(const std::filesystem::path& path) {
    return enqueue(path, true);
}

bool ImageCache::enqueue(const std::filesystem::path& path, const bool force) {
    if (path.empty())
        return false;

    {
        std::lock_guard lock{mutex_};
        auto& entry = entries_[path];
        entry.pinned = pinnedPaths_.contains(path);
        if (!force && entry.status != ImageStatus::Missing && entry.status != ImageStatus::Failed)
            return false;
        if (entry.status == ImageStatus::Loading || entry.status == ImageStatus::AwaitingUpload)
            return false;

        entry.status = ImageStatus::Loading;
        entry.error.clear();
        ++entry.generation;
        jobs_.push_back({path, entry.generation});
    }
    wake_.notify_one();
    return true;
}

void ImageCache::workerLoop() {
    while (true) {
        DecodeJob job;
        bool ready = false;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty())
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++activeJobs_;
        }

        auto decoded = [&]() -> Result<DecodedImage> {
            try {
                return decoder_(job.path);
            } catch (const std::exception& error) {
                return Error{job.path.string(), "decoder threw an exception: " + std::string{error.what()}};
            } catch (...) {
                return Error{job.path.string(), "decoder threw an unknown exception"};
            }
        }();

        {
            std::lock_guard lock{mutex_};
            --activeJobs_;
            auto found = entries_.find(job.path);
            if (found != entries_.end() && found->second.generation == job.generation) {
                auto& entry = found->second;
                if (!decoded) {
                    entry.status = entry.texture ? ImageStatus::Ready : ImageStatus::Failed;
                    entry.error = decoded.error().message;
                    ready = true;
                } else if (const auto error = validate(decoded.value())) {
                    entry.status = entry.texture ? ImageStatus::Ready : ImageStatus::Failed;
                    entry.error = *error;
                    ready = true;
                } else {
                    entry.decoded = std::move(decoded.value());
                    entry.status = ImageStatus::AwaitingUpload;
                    entry.error.clear();
                    ready = true;
                }
            }
            if (jobs_.empty() && activeJobs_ == 0)
                idle_.notify_all();
        }
        if (ready && readyCallback_)
            readyCallback_();
    }
}

std::vector<UploadRequest> ImageCache::takeUploads(const std::size_t maxCount) {
    std::lock_guard lock{mutex_};
    std::vector<UploadRequest> result;
    for (auto& [path, entry] : entries_) {
        if (result.size() >= maxCount)
            break;
        if (entry.status != ImageStatus::AwaitingUpload || !entry.decoded)
            continue;
        result.push_back({path, entry.generation, std::move(*entry.decoded), entry.texture});
        entry.decoded.reset();
        entry.status = ImageStatus::Loading;
    }
    return result;
}

bool ImageCache::hasPendingUploads() const {
    std::lock_guard lock{mutex_};
    return std::ranges::any_of(entries_, [](const auto& item) { return item.second.status == ImageStatus::AwaitingUpload; });
}

bool ImageCache::completeUpload(const UploadRequest& request, TextureHandle texture, const std::size_t textureBytes) {
    if (!texture || textureBytes == 0)
        return failUpload(request, "texture upload returned an empty texture");

    std::lock_guard lock{mutex_};
    const auto found = entries_.find(request.path);
    if (found == entries_.end() || found->second.generation != request.generation)
        return false;

    auto& entry = found->second;
    residentBytes_ -= entry.textureBytes;
    entry.texture = std::move(texture);
    entry.textureBytes = textureBytes;
    residentBytes_ += textureBytes;
    entry.status = ImageStatus::Ready;
    entry.error.clear();
    entry.lastUsed = ++clock_;
    evictLocked();
    pruneMetadataLocked();
    return true;
}

bool ImageCache::failUpload(const UploadRequest& request, std::string message) {
    std::lock_guard lock{mutex_};
    const auto found = entries_.find(request.path);
    if (found == entries_.end() || found->second.generation != request.generation)
        return false;
    auto& entry = found->second;
    entry.status = entry.texture ? ImageStatus::Ready : ImageStatus::Failed;
    entry.error = std::move(message);
    return true;
}

TextureHandle ImageCache::texture(const std::filesystem::path& path) {
    std::lock_guard lock{mutex_};
    const auto found = entries_.find(path);
    if (found == entries_.end() || !found->second.texture)
        return {};
    found->second.lastUsed = ++clock_;
    return found->second.texture;
}

ImageSnapshot ImageCache::snapshot(const std::filesystem::path& path) const {
    std::lock_guard lock{mutex_};
    const auto found = entries_.find(path);
    if (found == entries_.end())
        return {};
    const auto& entry = found->second;
    return {
        .status = entry.status,
        .texture = entry.texture,
        .textureBytes = entry.textureBytes,
        .error = entry.error,
        .generation = entry.generation,
        .pinned = entry.pinned,
    };
}

void ImageCache::setPinned(const std::set<std::filesystem::path>& paths) {
    std::lock_guard lock{mutex_};
    pinnedPaths_ = paths;
    for (auto& [path, entry] : entries_)
        entry.pinned = paths.contains(path);
    evictLocked();
    pruneMetadataLocked();
}

void ImageCache::setBudget(const std::size_t budgetBytes) {
    std::lock_guard lock{mutex_};
    budgetBytes_ = budgetBytes;
    evictLocked();
    pruneMetadataLocked();
}

void ImageCache::evictLocked() {
    while (residentBytes_ > budgetBytes_) {
        auto victim = entries_.end();
        for (auto current = entries_.begin(); current != entries_.end(); ++current) {
            if (!current->second.texture || current->second.pinned)
                continue;
            if (victim == entries_.end() || current->second.lastUsed < victim->second.lastUsed)
                victim = current;
        }
        if (victim == entries_.end())
            return;

        residentBytes_ -= victim->second.textureBytes;
        victim->second.texture.reset();
        victim->second.textureBytes = 0;
        victim->second.status = ImageStatus::Missing;
    }
}

void ImageCache::pruneMetadataLocked() {
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        const auto& entry = iterator->second;
        const bool active = entry.pinned || pinnedPaths_.contains(iterator->first) || entry.texture || entry.decoded ||
            entry.status == ImageStatus::Loading || entry.status == ImageStatus::AwaitingUpload;
        if (active)
            ++iterator;
        else
            iterator = entries_.erase(iterator);
    }
}

std::size_t ImageCache::residentBytes() const {
    std::lock_guard lock{mutex_};
    return residentBytes_;
}

std::size_t ImageCache::budgetBytes() const {
    std::lock_guard lock{mutex_};
    return budgetBytes_;
}

void ImageCache::waitForIdle() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return jobs_.empty() && activeJobs_ == 0; });
}

} // namespace luxaxis
