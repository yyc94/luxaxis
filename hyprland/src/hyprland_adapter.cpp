#include "hyprland_adapter.hpp"

#include "luxaxis/gpu_renderer.hpp"

#include <src/Compositor.hpp>
#include <src/SharedDefs.hpp>
#include <src/debug/log/Logger.hpp>
#include <src/desktop/Workspace.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/desktop/view/Window.hpp>
#include <src/event/EventBus.hpp>
#include <src/helpers/math/Math.hpp>
#include <src/managers/fullscreen/FullscreenController.hpp>
#include <src/output/Monitor.hpp>
#include <src/pointer/PointerManager.hpp>
#include <src/render/Renderer.hpp>
#include <src/render/OpenGL.hpp>
#include <src/render/pass/RectPassElement.hpp>
#include <src/render/pass/TexPassElement.hpp>
#include <src/state/MonitorState.hpp>

#include <hyprgraphics/image/Image.hpp>
#include <drm_fourcc.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace luxaxis::hyprland {
namespace {

using namespace std::chrono_literals;

struct HyprTexture {
    SP<Render::ITexture> texture;
};

TextureHandle wrapTexture(SP<Render::ITexture> texture) {
    if (!texture)
        return {};
    return std::make_shared<HyprTexture>(HyprTexture{std::move(texture)});
}

SP<Render::ITexture> unwrapTexture(const TextureHandle& handle) {
    if (!handle)
        return {};
    return std::static_pointer_cast<HyprTexture>(handle)->texture;
}

Result<DecodedImage> decodeImage(const std::filesystem::path& path) {
    Hyprgraphics::CImage image{path.string()};
    if (!image.success())
        return Error{path.string(), image.getError()};

    const auto surface = image.cairoSurface();
    if (!surface)
        return Error{path.string(), "decoder returned no Cairo surface"};

    const auto size = surface->size();
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0.0 || size.y <= 0.0 ||
        size.x > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        size.y > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) || !surface->data())
        return Error{path.string(), "decoder returned invalid image dimensions or data"};
    const auto width = static_cast<std::uint32_t>(size.x);
    const auto height = static_cast<std::uint32_t>(size.y);
    const auto sourceStride = surface->stride();
    if (width == 0 || height == 0 || width > std::numeric_limits<std::uint32_t>::max() / 4U || sourceStride <= 0)
        return Error{path.string(), "decoder returned an invalid Cairo surface"};
    const auto minimumStride = static_cast<std::uint64_t>(width) * 4ULL;
    if (static_cast<std::uint64_t>(sourceStride) < minimumStride || static_cast<std::uint64_t>(height) > std::numeric_limits<std::uint64_t>::max() / minimumStride)
        return Error{path.string(), "decoder returned an invalid Cairo surface"};
    const auto pixelBytes = minimumStride * static_cast<std::uint64_t>(height);
    if (pixelBytes > std::numeric_limits<std::size_t>::max())
        return Error{path.string(), "decoder returned an invalid Cairo surface"};

    DecodedImage result{
        .width = width,
        .height = height,
        .stride = width * 4U,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(pixelBytes)),
    };

    const auto* source = surface->data();
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* sourceRow = source + static_cast<std::size_t>(y) * sourceStride;
        auto* destinationRow = result.rgba.data() + static_cast<std::size_t>(y) * result.stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto* pixel = sourceRow + x * 4U;
            auto* destination = destinationRow + x * 4U;
            destination[0] = pixel[2];
            destination[1] = pixel[1];
            destination[2] = pixel[0];
            destination[3] = pixel[3];
        }
    }
    return result;
}

Color parseFallbackColor() {
    return {0.0, 0.0, 0.0};
}

class GpuTexture final : public Render::ITexture {
  public:
    explicit GpuTexture(std::shared_ptr<const gpu::Texture> texture) : texture_(std::move(texture)) {
        m_texID = texture_->id();
        m_size = {texture_->size().x, texture_->size().y};
        m_drmFormat = DRM_FORMAT_ABGR8888;
        m_opaque = true;
        m_imageDescription = NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION;
    }
    void setTexParameter(GLenum name, GLint value) override { glTexParameteri(GL_TEXTURE_2D, name, value); }
    void allocate(const Vector2D&, uint32_t) override { throw std::logic_error("Luxaxis GPU texture is read-only"); }
    void update(uint32_t, uint8_t*, uint32_t, const CRegion&) override { throw std::logic_error("Luxaxis GPU texture is read-only"); }
    void bind() override { glBindTexture(GL_TEXTURE_2D, m_texID); }
    void unbind() override { glBindTexture(GL_TEXTURE_2D, 0); }
    bool ok() override { return m_texID != 0; }
    bool isDMA() override { return false; }

  private:
    std::shared_ptr<const gpu::Texture> texture_;
};

class WallpaperPass final : public IPassElement {
  public:
    WallpaperPass(const Vec2 size, std::function<std::vector<UP<IPassElement>>()> draw) : size_(size), draw_(std::move(draw)) {}
    std::vector<UP<IPassElement>> draw() override { return draw_(); }
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "LuxaxisWallpaper"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override { return CBox{0, 0, size_.x, size_.y}; }
    CRegion opaqueRegion() override { return CRegion{CBox{0, 0, size_.x, size_.y}}; }

  private:
    Vec2 size_;
    std::function<std::vector<UP<IPassElement>>()> draw_;
};

std::chrono::milliseconds engineTime() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

bool wallpaperOccluded(PHLMONITOR monitor) {
    const auto window = Fullscreen::controller()->getFullscreenWindow(monitor);
    if (!window || !window->opaque())
        return false;
    const auto position = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto size = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    return position.x <= monitor->m_position.x && position.y <= monitor->m_position.y &&
        position.x + size.x >= monitor->m_position.x + monitor->m_size.x &&
        position.y + size.y >= monitor->m_position.y + monitor->m_size.y;
}

class MainThreadWake {
  public:
    explicit MainThreadWake(std::function<void()> callback) : callback_(std::move(callback)) {
        if (pipe2(fds_, O_NONBLOCK | O_CLOEXEC) != 0)
            throw std::system_error(errno, std::generic_category(), "failed to create Luxaxis wake pipe");
        source_ = wl_event_loop_add_fd(
            g_pCompositor->m_wlEventLoop,
            fds_[0],
            WL_EVENT_READABLE,
            [](int fd, uint32_t, void* data) {
                auto* wake = static_cast<MainThreadWake*>(data);
                std::uint8_t buffer[64];
                while (read(fd, buffer, sizeof(buffer)) > 0) {
                }
                if (wake->callback_)
                    wake->callback_();
                return 0;
            },
            this);
        if (!source_) {
            close(fds_[0]);
            close(fds_[1]);
            fds_[0] = -1;
            fds_[1] = -1;
            throw std::runtime_error("failed to register Luxaxis wake source");
        }
    }

    ~MainThreadWake() {
        if (source_)
            wl_event_source_remove(source_);
        if (fds_[0] >= 0)
            close(fds_[0]);
        if (fds_[1] >= 0)
            close(fds_[1]);
    }

    void signal() const {
        if (fds_[1] < 0)
            return;
        const std::uint8_t byte = 1;
        (void)write(fds_[1], &byte, sizeof(byte));
    }

  private:
    int fds_[2] = {-1, -1};
    wl_event_source* source_ = nullptr;
    std::function<void()> callback_;
};

Config fallbackConfig() {
    Config config;
    config.defaultProfile = "default";
    config.fallbackColor = parseFallbackColor();
    Profile profile;
    profile.wallpaper = "/__luxaxis_missing_wallpaper__.png";
    config.profiles.emplace("default", std::move(profile));
    return config;
}

std::string_view imageStatusName(const ImageStatus status) {
    switch (status) {
        case ImageStatus::Missing: return "missing";
        case ImageStatus::Loading: return "loading";
        case ImageStatus::AwaitingUpload: return "awaiting-upload";
        case ImageStatus::Ready: return "ready";
        case ImageStatus::Failed: return "failed";
    }
    return "unknown";
}

std::int64_t normalWorkspace(PHLWORKSPACE workspace) {
    if (!workspace || workspace->m_isSpecialWorkspace || workspace->m_id <= 0)
        return 0;
    return workspace->m_id;
}

CHyprColor hyprColor(const Color color, const float alpha = 1.F) {
    return {static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), alpha};
}

class ConfigWatcher {
  public:
    ConfigWatcher(std::filesystem::path source, std::filesystem::path home, std::function<void()> wake = {})
        : source_(std::move(source)), home_(std::move(home)), wake_(std::move(wake)) {
        thread_ = std::thread([this] { run(); });
    }

    ~ConfigWatcher() {
        stopping_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    void requestReload() {
        forceReload_ = true;
    }

    std::optional<Config> takeConfig(std::optional<Error>& error) {
        std::lock_guard lock{mutex_};
        error = std::move(error_);
        error_.reset();
        auto result = std::move(pending_);
        pending_.reset();
        return result;
    }

  private:
    void run() {
        std::optional<std::filesystem::file_time_type> observed;
        std::optional<std::filesystem::file_time_type> pendingChange;
        auto pendingSince = std::chrono::steady_clock::now();
        while (!stopping_) {
            std::error_code ec;
            const auto current = std::filesystem::last_write_time(source_, ec);
            const auto forced = forceReload_.exchange(false);
            bool changed = forced;
            if (!forced && !ec && (!observed || current != *observed)) {
                if (!pendingChange || current != *pendingChange) {
                    pendingChange = current;
                    pendingSince = std::chrono::steady_clock::now();
                } else if (std::chrono::steady_clock::now() - pendingSince >= 300ms) {
                    changed = true;
                }
            } else if (!forced && !ec) {
                pendingChange.reset();
            }
            if (changed) {
                observed = ec ? std::optional<std::filesystem::file_time_type>{} : std::optional{current};
                pendingChange.reset();
                auto loaded = loadConfig(source_, home_);
                std::lock_guard lock{mutex_};
                if (loaded)
                    pending_ = std::move(loaded.value());
                else
                    error_ = loaded.error();
                if (wake_)
                    wake_();
            }
            std::this_thread::sleep_for(300ms);
        }
    }

    std::filesystem::path source_;
    std::filesystem::path home_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> forceReload_{true};
    std::mutex mutex_;
    std::optional<Config> pending_;
    std::optional<Error> error_;
    std::function<void()> wake_;
};

std::set<std::filesystem::path> wallpaperPaths(const Config& config) {
    std::set<std::filesystem::path> result;
    for (const auto& [unused, profile] : config.profiles) {
        (void)unused;
        result.insert(profile.wallpaper);
    }
    return result;
}

class WallpaperWatcher {
  public:
    WallpaperWatcher(ImageCache& cache, const Config& config, std::function<void()> wake = {})
        : cache_(cache), paths_(wallpaperPaths(config)), wake_(std::move(wake)) {
        thread_ = std::thread([this] { run(); });
    }

    ~WallpaperWatcher() {
        stopping_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    void setPaths(const Config& config) {
        std::lock_guard lock{mutex_};
        paths_ = wallpaperPaths(config);
    }

  private:
    void run() {
        std::map<std::filesystem::path, std::optional<std::filesystem::file_time_type>> observed;
        while (!stopping_) {
            std::set<std::filesystem::path> paths;
            {
                std::lock_guard lock{mutex_};
                paths = paths_;
            }

            for (auto iterator = observed.begin(); iterator != observed.end();) {
                if (!paths.contains(iterator->first))
                    iterator = observed.erase(iterator);
                else
                    ++iterator;
            }

            for (const auto& path : paths) {
                std::error_code ec;
                const auto current = std::filesystem::last_write_time(path, ec);
                const std::optional currentTime = ec ? std::nullopt : std::optional{current};
                const auto entry = observed.find(path);
                if (entry == observed.end()) {
                    observed.emplace(path, currentTime);
                } else if (entry->second != currentTime) {
                    if (cache_.refresh(path)) {
                        entry->second = currentTime;
                        if (wake_)
                            wake_();
                    }
                }
            }
            std::this_thread::sleep_for(300ms);
        }
    }

    ImageCache& cache_;
    std::set<std::filesystem::path> paths_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::function<void()> wake_;
};

} // namespace

struct Adapter::Impl {
    HANDLE handle;
    std::filesystem::path configPath;
    std::filesystem::path home;
    Config config;
    Engine engine;
    MainThreadWake wake;
    ImageCache cache;
    ConfigWatcher watcher;
    WallpaperWatcher wallpaperWatcher;
    std::mutex errorMutex;
    std::optional<Error> lastConfigError;
    bool usingFallbackConfig = false;
    std::unordered_map<std::string, std::unique_ptr<gpu::Renderer>> gpuRenderers;
    std::unordered_map<std::string, SP<Render::ITexture>> gpuTextures;
    std::unordered_map<std::string, std::string> gpuErrors;
    struct LastWallpaper {
        std::filesystem::path path;
        Profile profile;
    };
    std::unordered_map<std::string, LastWallpaper> lastWallpaperTextures;
    struct ReplacementFade {
        std::chrono::steady_clock::time_point started;
        std::uint64_t id;
    };
    std::unordered_map<std::filesystem::path, ReplacementFade> replacementFades;
    std::unordered_set<std::filesystem::path> reportedImageErrors;
    std::unordered_map<std::string, std::string> outputStates;
    CHyprSignalListener renderStage;
    CHyprSignalListener workspaceActive;
    CHyprSignalListener workspaceMove;
    CHyprSignalListener monitorAdded;
    CHyprSignalListener monitorRemoved;
    CHyprSignalListener monitorFocused;
    CHyprSignalListener cursorMove;
    SP<SHyprCtlCommand> reloadCommand;
    SP<SHyprCtlCommand> spotlightCommand;
    SP<SHyprCtlCommand> statusCommand;
    std::uint64_t nextReplacementId = 1ULL << 63U;

    Impl(HANDLE pluginHandle, std::filesystem::path source, std::filesystem::path homePath, Config initial)
        : handle(pluginHandle), configPath(std::move(source)), home(std::move(homePath)), config(std::move(initial)), engine(config),
          wake([this] { scheduleManagedFrames(); }),
          cache(config.textureCacheBytes, decodeImage, [this] { wake.signal(); }),
          watcher(configPath, home, [this] { wake.signal(); }),
          wallpaperWatcher(cache, config, [this] { wake.signal(); }) {
        syncOutputs();

        renderStage = Event::bus()->m_events.render.stage.listen([this](eRenderStage stage) {
            if (stage == RENDER_POST_WALLPAPER)
                renderWallpaper();
        });
        workspaceActive = Event::bus()->m_events.workspace.active.listen([this](PHLWORKSPACE workspace) {
            if (workspace) {
                if (auto monitor = workspace->m_monitor.lock()) {
                    syncOutput(monitor);
                    damageOutput(monitor->m_name);
                }
            } else {
                damageManagedOutputs();
            }
        });
        workspaceMove = Event::bus()->m_events.workspace.moveToMonitor.listen([this](PHLWORKSPACE workspace, PHLMONITOR monitor) {
            if (workspace && monitor) {
                syncOutput(monitor);
                damageOutput(monitor->m_name);
            } else {
                damageManagedOutputs();
            }
        });
        monitorAdded = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR monitor) {
            syncOutput(monitor);
            damageManagedOutputs();
        });
        monitorRemoved = Event::bus()->m_events.monitor.removed.listen([this](PHLMONITOR monitor) {
            if (monitor) {
                (void)engine.removeOutput(monitor->m_name);
                lastWallpaperTextures.erase(monitor->m_name);
                if (Render::GL::g_pHyprOpenGL)
                    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
                gpuTextures.erase(monitor->m_name);
                gpuRenderers.erase(monitor->m_name);
                gpuErrors.erase(monitor->m_name);
                outputStates.erase(monitor->m_name);
            }
            damageManagedOutputs();
        });
        monitorFocused = Event::bus()->m_events.monitor.focused.listen([this](PHLMONITOR monitor) {
            const auto previous = engine.activeOutput();
            (void)engine.setFocusedOutput(monitor ? std::optional{monitor->m_name} : std::nullopt);
            if (previous)
                damageOutput(*previous);
            if (const auto current = engine.activeOutput())
                damageOutput(*current);
        });
        cursorMove = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D position, Event::SCallbackInfo&) {
            const auto previous = engine.activeOutput();
            if (!engine.setCursor({position.x, position.y}))
                return;
            const auto current = engine.activeOutput();
            if (previous != current) {
                if (previous)
                    damageOutput(*previous);
                if (current)
                    damageOutput(*current);
                return;
            }
            if (!current)
                return;

            const auto after = engine.planFor(*current);
            if (!after)
                return;
            if (after->transitioning) {
                damageOutput(*current);
                return;
            }

            const auto renderer = gpuRenderers.find(*current);
            if (renderer == gpuRenderers.end())
                return;
            const auto bounds = renderer->second->cursorDamage(after->cursorLocal);
            if (!bounds)
                return;
            const CRegion changedRegion{CBox{
                after->logicalBounds.position.x + bounds->position.x,
                after->logicalBounds.position.y + bounds->position.y,
                bounds->size.x,
                bounds->size.y,
            }};
            const auto monitor = std::ranges::find_if(
                State::monitorState()->monitors(), [&current](const auto& candidate) { return candidate && candidate->m_name == *current; });
            if (!changedRegion.empty() && monitor != State::monitorState()->monitors().end() &&
                !wallpaperOccluded(*monitor))
                g_pHyprRenderer->damageRegion(changedRegion);
        });
    }

    ~Impl() {
        reloadCommand.reset();
        spotlightCommand.reset();
        renderStage.reset();
        workspaceActive.reset();
        workspaceMove.reset();
        monitorAdded.reset();
        monitorRemoved.reset();
        monitorFocused.reset();
        cursorMove.reset();
        if (Render::GL::g_pHyprOpenGL)
            Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        if (g_pHyprRenderer)
            g_pHyprRenderer->currentPass().removeAllOfType("LuxaxisWallpaper");
        gpuTextures.clear();
        gpuRenderers.clear();
        replacementFades.clear();
    }

    void syncOutput(PHLMONITOR monitor) {
        if (!monitor)
            return;
        engine.advanceTo(engineTime());
        const auto workspace = normalWorkspace(monitor->m_activeWorkspace);
        (void)engine.upsertOutput({
            .name = monitor->m_name,
            .logicalBounds = {{monitor->m_position.x, monitor->m_position.y}, {monitor->m_size.x, monitor->m_size.y}},
            .workspace = workspace > 0 ? std::optional{workspace} : std::nullopt,
        });
        if (const auto plan = engine.planFor(monitor->m_name))
            engine.setTransitionReady(monitor->m_name, static_cast<bool>(cache.texture(plan->profile.wallpaper)));
    }

    void syncOutputs() {
        for (const auto& monitor : State::monitorState()->monitors())
            syncOutput(monitor);
        if (const auto monitor = Desktop::focusState()->monitor())
            (void)engine.setFocusedOutput(monitor->m_name);
        const auto position = Pointer::mgr()->position();
        (void)engine.setCursor({position.x, position.y});
    }

    void damageManagedOutputs() {
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && !wallpaperOccluded(monitor) && engine.planFor(monitor->m_name))
                g_pHyprRenderer->damageMonitor(monitor);
    }

    void scheduleManagedFrames() {
        damageManagedOutputs();
    }

    void damageOutput(const std::string& name) {
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && monitor->m_name == name && !wallpaperOccluded(monitor) && engine.planFor(name)) {
                g_pHyprRenderer->damageMonitor(monitor);
                return;
            }
    }

    void applyPendingConfig() {
        std::optional<Error> error;
        if (auto candidate = watcher.takeConfig(error)) {
            config = std::move(*candidate);
            engine.applyConfig(config);
            for (const auto& plan : engine.plans())
                engine.setTransitionReady(plan.output, static_cast<bool>(cache.texture(plan.profile.wallpaper)));
            cache.setBudget(config.textureCacheBytes);
            wallpaperWatcher.setPaths(config);
            for (const auto& excluded : config.excludedOutputs) {
                gpuTextures.erase(excluded);
                gpuRenderers.erase(excluded);
                gpuErrors.erase(excluded);
                lastWallpaperTextures.erase(excluded);
                outputStates.erase(excluded);
                for (const auto& monitor : State::monitorState()->monitors())
                    if (monitor && monitor->m_name == excluded)
                        g_pHyprRenderer->damageMonitor(monitor);
            }
            const auto paths = wallpaperPaths(config);
            std::erase_if(reportedImageErrors, [&paths](const auto& path) { return !paths.contains(path); });
            usingFallbackConfig = false;
            lastConfigError.reset();
            Log::logger->log(Log::DEBUG, "Luxaxis config loaded: {} ({} profiles, {} workspace mappings)",
                             configPath.string(), config.profiles.size(), config.workspaces.size());
            damageManagedOutputs();
        }
        if (error) {
            std::lock_guard lock{errorMutex};
            if (!lastConfigError || lastConfigError->message != error->message)
                Log::logger->log(Log::ERR, "Luxaxis config {}: {}", error->source, error->message);
            lastConfigError = std::move(error);
        }
    }
    void reportImageError(const std::filesystem::path& path, const std::string& message) {
        if (reportedImageErrors.insert(path).second)
            Log::logger->log(Log::ERR, "Luxaxis wallpaper {}: {}", path.string(), message);
    }

    void reportOutputState(const std::string& output, std::string state, const bool failure) {
        if (const auto previous = outputStates.find(output); previous != outputStates.end() && previous->second == state)
            return;
        outputStates.insert_or_assign(output, state);
        Log::logger->log(failure ? Log::ERR : Log::DEBUG, "Luxaxis output {}: {}", output, state);
    }

    TextureHandle wallpaperTexture(const std::vector<std::filesystem::path>& candidates, std::filesystem::path* selectedPath = nullptr) {
        for (const auto& candidate : candidates) {
            const auto snapshot = cache.snapshot(candidate);
            if (!snapshot.error.empty()) {
                reportImageError(candidate, snapshot.error);
            } else {
                reportedImageErrors.erase(candidate);
            }
            if (auto current = cache.texture(candidate)) {
                if (selectedPath)
                    *selectedPath = candidate;
                return current;
            }
            (void)cache.request(candidate);
        }
        return {};
    }

    void uploadPendingTextures() {
        const auto now = std::chrono::steady_clock::now();
        for (auto iterator = replacementFades.begin(); iterator != replacementFades.end();) {
            if (now - iterator->second.started >= 120ms)
                iterator = replacementFades.erase(iterator);
            else
                ++iterator;
        }
        for (auto& request : cache.takeUploads(1)) {
            GLint limit = 0;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
            if (limit <= 0 || request.image.width > static_cast<std::uint32_t>(limit) || request.image.height > static_cast<std::uint32_t>(limit)) {
                const std::string message = "wallpaper dimensions exceed the OpenGL texture size limit";
                if (cache.failUpload(request, message))
                    reportImageError(request.path, message);
                continue;
            }
            const auto texture = g_pHyprRenderer->createTexture(
                DRM_FORMAT_ABGR8888,
                request.image.rgba.data(),
                request.image.stride,
                {static_cast<double>(request.image.width), static_cast<double>(request.image.height)},
                false,
                false);
            if (!texture) {
                const std::string message = "Hyprland rejected the wallpaper texture upload";
                if (cache.failUpload(request, message))
                    reportImageError(request.path, message);
                continue;
            }
            texture->m_imageDescription = NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION;
            const auto bytes = static_cast<std::size_t>(request.image.stride) * static_cast<std::size_t>(request.image.height);
            if (cache.completeUpload(request, wrapTexture(texture), bytes)) {
                reportedImageErrors.erase(request.path);
                Log::logger->log(Log::DEBUG, "Luxaxis wallpaper ready: {} ({}x{})", request.path.string(),
                                 request.image.width, request.image.height);
                if (request.previousTexture)
                    replacementFades.insert_or_assign(request.path, ReplacementFade{now, nextReplacementId++});
            }
        }
        if (cache.hasPendingUploads())
            damageManagedOutputs();
    }

    std::vector<UP<IPassElement>> drawGpuWallpaper(const std::string& output, const gpu::Input& input) {
        std::vector<UP<IPassElement>> result;
        SP<Render::ITexture> texture;
        try {
            if (g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL)
                throw std::runtime_error("Luxaxis requires Hyprland's OpenGL renderer");
            auto& renderer = gpuRenderers[output];
            if (!renderer)
                renderer = std::make_unique<gpu::Renderer>();
            const auto rendered = renderer->render(input);
            auto& wrapper = gpuTextures[output];
            const bool firstFrame = !wrapper;
            if (!wrapper || wrapper->m_texID != rendered->id())
                wrapper = makeShared<GpuTexture>(rendered);
            texture = wrapper;
            const bool recovered = gpuErrors.erase(output) != 0;
            if (firstFrame || recovered)
                Log::logger->log(Log::DEBUG, "Luxaxis GPU output {}: rendering ready", output);
        } catch (const std::exception& error) {
            if (gpuErrors[output] != error.what()) {
                Log::logger->log(Log::ERR, "Luxaxis GPU {}: {}", output, error.what());
                gpuErrors[output] = error.what();
            }
            if (const auto previous = gpuTextures.find(output); previous != gpuTextures.end())
                texture = previous->second;
        }
        if (texture) {
            CTexPassElement::SRenderData data{};
            data.tex = texture;
            data.box = {0, 0, input.pixelSize.x, input.pixelSize.y};
            result.push_back(makeUnique<CTexPassElement>(std::move(data)));
        } else {
            CRectPassElement::SRectData data{};
            data.box = {0, 0, input.pixelSize.x, input.pixelSize.y};
            data.color = hyprColor(input.fallbackColor);
            result.push_back(makeUnique<CRectPassElement>(std::move(data)));
        }
        return result;
    }

    void renderWallpaper() {
        const auto now = std::chrono::steady_clock::now();
        engine.advanceTo(engineTime());
        applyPendingConfig();
        auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor)
            return;
        uploadPendingTextures();
        syncOutput(monitor);
        auto plan = engine.planFor(monitor->m_name);
        if (!plan)
            return;

        std::set<std::filesystem::path> pinned;
        for (const auto& other : engine.plans()) {
            for (const auto& candidate : other.wallpaperCandidates)
                pinned.insert(candidate);
        }
        for (const auto& [unused, last] : lastWallpaperTextures) {
            (void)unused;
            pinned.insert(last.path);
        }
        cache.setPinned(pinned);

        std::filesystem::path selectedPath;
        auto wallpaper = wallpaperTexture({plan->profile.wallpaper}, &selectedPath);
        const auto destination = cache.snapshot(plan->profile.wallpaper);
        Profile selectedProfile = config.profiles.at(plan->profileName);
        if (!wallpaper && destination.status == ImageStatus::Failed) {
            engine.cancelTransition(monitor->m_name);
            plan.emplace(*engine.planFor(monitor->m_name));
            selectedProfile = config.profiles.at(config.defaultProfile);
            wallpaper = wallpaperTexture({selectedProfile.wallpaper}, &selectedPath);
        }
        const bool ready = wallpaper || (destination.status == ImageStatus::Failed &&
            cache.snapshot(selectedProfile.wallpaper).status == ImageStatus::Failed);
        engine.setTransitionReady(monitor->m_name, ready);
        if (ready && !wallpaper)
            selectedProfile.spotlight = {};

        if (wallpaper) {
            reportOutputState(monitor->m_name,
                "workspace " + (plan->workspace ? std::to_string(*plan->workspace) : std::string{"none"}) +
                " profile " + plan->profileName + " wallpaper " + selectedPath.string() +
                (selectedPath == plan->profile.wallpaper ? " ready" : " using default profile"), false);
        } else {
            const auto selected = cache.snapshot(selectedProfile.wallpaper);
            const bool failed = destination.status == ImageStatus::Failed && selected.status == ImageStatus::Failed;
            const bool retainingPrevious = !ready && lastWallpaperTextures.contains(monitor->m_name) &&
                gpuTextures.contains(monitor->m_name);
            reportOutputState(monitor->m_name,
                "workspace " + (plan->workspace ? std::to_string(*plan->workspace) : std::string{"none"}) +
                " profile " + plan->profileName +
                (retainingPrevious ? " retaining previous wallpaper; wallpaper " : " fallback color; wallpaper ") +
                plan->profile.wallpaper.string() +
                " is " + std::string{imageStatusName(destination.status)} +
                (selectedProfile.wallpaper == plan->profile.wallpaper ? "" :
                    ", default " + selectedProfile.wallpaper.string() + " is " + std::string{imageStatusName(selected.status)}),
                failed);
        }

        const auto texture = unwrapTexture(wallpaper);
        gpu::Input input{
            .destination = texture ? gpu::Image{texture->m_texID, {texture->m_size.x, texture->m_size.y},
                                               selectedProfile.fit, selectedProfile.position, wallpaper} : gpu::Image{},
            .spotlight = selectedProfile.spotlight,
            .fallbackColor = plan->fallbackColor,
            .logicalSize = plan->logicalBounds.size,
            .pixelSize = {monitor->m_transformedSize.x, monitor->m_transformedSize.y},
            .cursor = plan->cursorLocal,
            .transitionOrigin = plan->transitionOrigin,
            .transition = plan->transition,
            .progress = plan->transitionProgress,
            .transitionId = plan->transitionId,
            .transitioning = plan->transitioning,
            .revealEnabled = engine.spotlightEnabled() && engine.activeOutput() == monitor->m_name,
            .spotlightEnabled = engine.spotlightEnabled(),
            .destinationReady = ready,
        };
        for (auto iterator = replacementFades.begin(); iterator != replacementFades.end();) {
            if (now - iterator->second.started >= 120ms)
                iterator = replacementFades.erase(iterator);
            else
                ++iterator;
        }
        if (!input.transitioning && !selectedPath.empty()) {
            if (const auto replacement = replacementFades.find(selectedPath); replacement != replacementFades.end()) {
                input.transitioning = true;
                input.transition = {.type = TransitionType::Fade, .durationMs = 120, .easing = "ease-out",
                                    .origin = TransitionOrigin::Center, .point = {0.5, 0.5}, .randomAllowlist = {}};
                input.progress = std::chrono::duration<double, std::milli>(now - replacement->second.started).count() / 120.0;
                input.transitionId = replacement->second.id;
            }
        }
        if (wallpaper)
            lastWallpaperTextures[monitor->m_name] = LastWallpaper{selectedPath, selectedProfile};

        // GL runs when this pass executes, after culling and at the wallpaper's
        // actual position in the render pass, not during pass construction.
        g_pHyprRenderer->addPassElement(makeUnique<WallpaperPass>(
            plan->logicalBounds.size, [this, output = monitor->m_name, input] { return drawGpuWallpaper(output, input); }));
        if (input.transitioning && ready && !wallpaperOccluded(monitor))
            g_pHyprRenderer->damageMonitor(monitor);
    }

    std::string status() {
        std::ostringstream result;
        result << "config=" << configPath.string() << " status=" << (usingFallbackConfig ? "fallback" : "loaded") << '\n';
        if (lastConfigError)
            result << "config_error=" << lastConfigError->source << ": " << lastConfigError->message << '\n';
        result << "active_output=" << engine.activeOutput().value_or("none") << '\n';
        bool foundOutput = false;
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!monitor)
                continue;
            foundOutput = true;
            result << "output=" << monitor->m_name;
            if (config.excludedOutputs.contains(monitor->m_name)) {
                result << " excluded\n";
                continue;
            }
            const auto plan = engine.planFor(monitor->m_name);
            if (!plan) {
                result << " no-render-plan\n";
                continue;
            }
            const auto image = cache.snapshot(plan->profile.wallpaper);
            result << " workspace=" << (plan->workspace ? std::to_string(*plan->workspace) : "none")
                   << " profile=" << plan->profileName << " wallpaper=" << plan->profile.wallpaper.string()
                   << " image=" << imageStatusName(image.status) << " texture=" << (image.texture ? "yes" : "no");
            if (const auto gpu = gpuErrors.find(monitor->m_name); gpu != gpuErrors.end())
                result << " gpu=error:" << gpu->second;
            else
                result << " gpu=" << (gpuTextures.contains(monitor->m_name) ? "ready" : "not-rendered");
            result << '\n';
            if (!image.error.empty())
                result << "image_error=" << image.error << '\n';
            if (const auto state = outputStates.find(monitor->m_name); state != outputStates.end())
                result << "display=" << state->second << '\n';
        }
        if (!foundOutput)
            result << "outputs=none\n";
        return result.str();
    }
};

Adapter::Adapter(HANDLE handle, std::filesystem::path configPath, std::filesystem::path home) {
    auto loaded = loadConfig(configPath, home);
    auto config = loaded ? std::move(loaded.value()) : fallbackConfig();
    if (!loaded)
        Log::logger->log(Log::ERR, "Luxaxis config {}: {}; using fallback color until a valid config is loaded", loaded.error().source, loaded.error().message);
    else
        Log::logger->log(Log::DEBUG, "Luxaxis config loaded: {} ({} profiles, {} workspace mappings)",
                         configPath.string(), config.profiles.size(), config.workspaces.size());
    impl_ = std::make_unique<Impl>(handle, std::move(configPath), std::move(home), std::move(config));
    if (!loaded) {
        impl_->usingFallbackConfig = true;
        impl_->lastConfigError = loaded.error();
    }
}

Adapter::~Adapter() = default;

SP<SHyprCtlCommand> Adapter::registerReloadCommand() {
    impl_->reloadCommand = HyprlandAPI::registerHyprCtlCommand(impl_->handle, {"luxaxis:reload", true, [this](eHyprCtlOutputFormat, std::string) { return reload(); }});
    return impl_->reloadCommand;
}

SP<SHyprCtlCommand> Adapter::registerSpotlightCommand() {
    impl_->spotlightCommand = HyprlandAPI::registerHyprCtlCommand(impl_->handle, {"luxaxis:spotlight", true, [this](eHyprCtlOutputFormat, std::string args) { return spotlight(std::move(args)); }});
    return impl_->spotlightCommand;
}

SP<SHyprCtlCommand> Adapter::registerStatusCommand() {
    impl_->statusCommand = HyprlandAPI::registerHyprCtlCommand(impl_->handle, {"luxaxis:status", true, [this](eHyprCtlOutputFormat, std::string) { return status(); }});
    return impl_->statusCommand;
}

void Adapter::unregisterCommands() {
    if (impl_->reloadCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(impl_->handle, impl_->reloadCommand);
        impl_->reloadCommand.reset();
    }
    if (impl_->spotlightCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(impl_->handle, impl_->spotlightCommand);
        impl_->spotlightCommand.reset();
    }
    if (impl_->statusCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(impl_->handle, impl_->statusCommand);
        impl_->statusCommand.reset();
    }
}

std::string Adapter::reload() {
    impl_->watcher.requestReload();
    return "reload queued";
}

std::string Adapter::spotlight(std::string args) {
    std::ranges::transform(args, args.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (args == "on")
        impl_->engine.spotlightOn();
    else if (args == "off")
        impl_->engine.spotlightOff();
    else if (args == "toggle" || args.empty())
        impl_->engine.spotlightToggle();
    else
        return "expected on, off, or toggle";
    impl_->damageManagedOutputs();
    return impl_->engine.spotlightEnabled() ? "on" : "off";
}

std::string Adapter::status() {
    return impl_->status();
}

} // namespace luxaxis::hyprland
