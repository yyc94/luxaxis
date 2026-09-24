#include "luxaxis/image_cache.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

luxaxis::DecodedImage image(const std::uint8_t value) {
    return {.width = 2, .height = 2, .stride = 8, .rgba = std::vector<std::uint8_t>(16, value)};
}

struct FakeDecoder {
    std::map<std::filesystem::path, bool> failures;
    std::atomic<bool> ranOffCallingThread{false};
    std::thread::id callingThread = std::this_thread::get_id();
    std::uint8_t generation = 1;

    luxaxis::Result<luxaxis::DecodedImage> operator()(const std::filesystem::path& path) {
        ranOffCallingThread = std::this_thread::get_id() != callingThread;
        if (failures[path])
            return luxaxis::Error{path.string(), "decode failed"};
        return image(generation++);
    }
};

std::shared_ptr<int> texture(const int id) {
    return std::make_shared<int>(id);
}

luxaxis::UploadRequest decode(luxaxis::ImageCache& cache, const std::filesystem::path& path) {
    require(cache.request(path), "image was not queued");
    cache.waitForIdle();
    auto uploads = cache.takeUploads();
    require(uploads.size() == 1, "decode did not produce one upload");
    return std::move(uploads.front());
}

void decodingIsOffThreadAndUploadIsExplicit() {
    FakeDecoder decoder;
    luxaxis::ImageCache cache{64, [&decoder](const auto& path) { return decoder(path); }};
    auto upload = decode(cache, "/wall/a.png");
    require(decoder.ranOffCallingThread, "decoder ran on the calling thread");
    require(cache.snapshot("/wall/a.png").status == luxaxis::ImageStatus::Loading, "upload was not handed to the render side");
    require(cache.completeUpload(upload, texture(1), 16), "upload completion was rejected");
    require(cache.snapshot("/wall/a.png").status == luxaxis::ImageStatus::Ready, "completed texture was not ready");
    require(cache.residentBytes() == 16, "texture bytes were not accounted");
}

void lruEvictsUnpinnedTextures() {
    FakeDecoder decoder;
    luxaxis::ImageCache cache{32, [&decoder](const auto& path) { return decoder(path); }};
    auto a = decode(cache, "/wall/a.png");
    require(cache.completeUpload(a, texture(1), 16), "first upload failed");
    auto b = decode(cache, "/wall/b.png");
    require(cache.completeUpload(b, texture(2), 16), "second upload failed");
    cache.setPinned({"/wall/a.png"});

    auto c = decode(cache, "/wall/c.png");
    require(cache.completeUpload(c, texture(3), 16), "third upload failed");
    require(cache.texture("/wall/a.png") != nullptr, "pinned texture was evicted");
    require(cache.texture("/wall/b.png") == nullptr, "least-recent unpinned texture survived eviction");
    require(cache.texture("/wall/c.png") != nullptr, "new texture was evicted instead of the LRU texture");
    require(cache.residentBytes() == 32, "cache did not return to budget");
}

void pinnedTexturesMayTemporarilyExceedBudget() {
    FakeDecoder decoder;
    luxaxis::ImageCache cache{16, [&decoder](const auto& path) { return decoder(path); }};
    auto a = decode(cache, "/wall/a.png");
    require(cache.completeUpload(a, texture(1), 16), "first upload failed");
    cache.setPinned({"/wall/a.png", "/wall/b.png"});
    auto b = decode(cache, "/wall/b.png");
    require(cache.completeUpload(b, texture(2), 16), "second upload failed");
    require(cache.residentBytes() == 32, "pinned textures did not exceed budget temporarily");
    cache.setPinned({"/wall/b.png"});
    require(cache.residentBytes() == 16 && cache.texture("/wall/a.png") == nullptr, "unpin did not settle cache to budget");
}

void failedRefreshRetainsLastValidTexture() {
    FakeDecoder decoder;
    luxaxis::ImageCache cache{64, [&decoder](const auto& path) { return decoder(path); }};
    auto first = decode(cache, "/wall/a.png");
    const auto original = texture(1);
    require(cache.completeUpload(first, original, 16), "initial upload failed");

    decoder.failures["/wall/a.png"] = true;
    require(cache.refresh("/wall/a.png"), "refresh was not queued");
    cache.waitForIdle();
    auto state = cache.snapshot("/wall/a.png");
    require(state.status == luxaxis::ImageStatus::Ready, "failed refresh discarded ready state");
    require(state.texture == original, "failed refresh discarded last valid texture");
    require(state.error == "decode failed", "failed refresh did not expose its diagnostic");

    decoder.failures["/wall/a.png"] = false;
    require(cache.refresh("/wall/a.png"), "replacement refresh was not queued");
    cache.waitForIdle();
    auto uploads = cache.takeUploads();
    require(uploads.size() == 1, "replacement did not produce an upload");
    require(uploads.front().previousTexture == original, "replacement did not retain the previous texture");
    const auto replacement = texture(2);
    require(cache.completeUpload(uploads.front(), replacement, 16), "replacement upload failed");
    require(cache.texture("/wall/a.png") == replacement, "replacement did not swap atomically");
    require(cache.residentBytes() == 16, "replacement double-counted texture bytes");
}

void invalidDecodeBufferIsRejected() {
    luxaxis::ImageCache cache{64, [](const auto&) -> luxaxis::Result<luxaxis::DecodedImage> {
                                   return luxaxis::DecodedImage{.width = 4, .height = 4, .stride = 4, .rgba = {0}};
                               }};
    require(cache.request("/wall/bad.png"), "invalid image was not queued");
    cache.waitForIdle();
    const auto state = cache.snapshot("/wall/bad.png");
    require(state.status == luxaxis::ImageStatus::Failed, "invalid decoded buffer was accepted");
    require(state.error.contains("stride"), "invalid buffer diagnostic was not concrete");
}

void decoderExceptionsBecomeFailures() {
    luxaxis::ImageCache cache{64, [](const auto&) -> luxaxis::Result<luxaxis::DecodedImage> { throw std::runtime_error("broken decoder"); }};
    require(cache.request("/wall/throw.png"), "throwing image was not queued");
    cache.waitForIdle();
    const auto state = cache.snapshot("/wall/throw.png");
    require(state.status == luxaxis::ImageStatus::Failed, "decoder exception escaped the worker boundary");
    require(state.error.contains("broken decoder"), "decoder exception lost its diagnostic");
}

void uploadsCanBeBudgetedAcrossFrames() {
    FakeDecoder decoder;
    luxaxis::ImageCache cache{64, [&decoder](const auto& path) { return decoder(path); }};
    require(cache.request("/wall/a.png"), "first image was not queued");
    require(cache.request("/wall/b.png"), "second image was not queued");
    cache.waitForIdle();

    auto first = cache.takeUploads(1);
    require(first.size() == 1, "upload budget was ignored");
    require(cache.hasPendingUploads(), "remaining upload was not reported");
    auto second = cache.takeUploads(1);
    require(second.size() == 1, "remaining upload was lost");
    require(!cache.hasPendingUploads(), "empty upload queue was reported as pending");
}

void inactiveFailureMetadataIsPruned() {
    FakeDecoder decoder;
    decoder.failures["/wall/removed.png"] = true;
    luxaxis::ImageCache cache{64, [&decoder](const auto& path) { return decoder(path); }};
    require(cache.request("/wall/removed.png"), "failed image was not queued");
    cache.waitForIdle();
    require(cache.snapshot("/wall/removed.png").status == luxaxis::ImageStatus::Failed, "failed image did not retain its diagnostic state");
    cache.setPinned({});
    require(cache.snapshot("/wall/removed.png").status == luxaxis::ImageStatus::Missing, "inactive failure metadata was not pruned");
}

} // namespace

int main() {
    try {
        decodingIsOffThreadAndUploadIsExplicit();
        lruEvictsUnpinnedTextures();
        pinnedTexturesMayTemporarilyExceedBudget();
        failedRefreshRetainsLastValidTexture();
        invalidDecodeBufferIsRejected();
        decoderExceptionsBecomeFailures();
        uploadsCanBeBudgetedAcrossFrames();
        inactiveFailureMetadataIsPruned();
    } catch (const std::exception& error) {
        std::cerr << "image_cache_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "image_cache_test: all checks passed\n";
    return EXIT_SUCCESS;
}
