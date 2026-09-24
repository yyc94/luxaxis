#include "hyprland_adapter.hpp"

#include "luxaxis/spotlight.hpp"
#include "luxaxis/transition.hpp"

#include <src/Compositor.hpp>
#include <src/SharedDefs.hpp>
#include <src/debug/log/Logger.hpp>
#include <src/desktop/Workspace.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/event/EventBus.hpp>
#include <src/helpers/math/Math.hpp>
#include <src/managers/fullscreen/FullscreenController.hpp>
#include <src/output/Monitor.hpp>
#include <src/pointer/PointerManager.hpp>
#include <src/render/Renderer.hpp>
#include <src/render/pass/ClearPassElement.hpp>
#include <src/render/pass/RectPassElement.hpp>
#include <src/render/pass/TexPassElement.hpp>
#include <src/state/MonitorState.hpp>

#include <hyprgraphics/image/Image.hpp>
#include <drm_fourcc.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
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
    const auto minimumStride = static_cast<std::uint64_t>(width) * 4ULL;
    const auto pixelBytes = static_cast<std::uint64_t>(width) * height * 4ULL;
    if (width == 0 || height == 0 || sourceStride <= 0 || static_cast<std::uint64_t>(sourceStride) < minimumStride ||
        pixelBytes > std::numeric_limits<std::size_t>::max())
        return Error{path.string(), "decoder returned an invalid Cairo surface"};

    DecodedImage result{
        .width = width,
        .height = height,
        .stride = width * 4U,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U),
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

Length interpolateLength(const Length& from, const Length& to, const double progress) {
    if (from.unit != to.unit)
        return progress < 0.5 ? from : to;
    return {std::lerp(from.value, to.value, progress), from.unit};
}

Spotlight interpolateSpotlight(const Spotlight& from, const Spotlight& to, const double progress) {
    Spotlight result = to;
    result.maskColor = {
        std::lerp(from.maskColor.r, to.maskColor.r, progress),
        std::lerp(from.maskColor.g, to.maskColor.g, progress),
        std::lerp(from.maskColor.b, to.maskColor.b, progress),
    };
    result.maskOpacity = std::lerp(from.maskOpacity, to.maskOpacity, progress);
    result.radius = interpolateLength(from.radius, to.radius, progress);
    result.softness = interpolateLength(from.softness, to.softness, progress);
    result.thickness = interpolateLength(from.thickness, to.thickness, progress);
    result.anchor = {
        std::lerp(from.anchor.x, to.anchor.x, progress),
        std::lerp(from.anchor.y, to.anchor.y, progress),
    };
    result.aspectRatio = std::lerp(from.aspectRatio, to.aspectRatio, progress);
    result.beamStartReveal = std::lerp(from.beamStartReveal, to.beamStartReveal, progress);
    if (progress < 0.5)
        result.orientation = from.orientation;
    return result;
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
    std::unordered_map<std::string, TextureHandle> maskTextures;
    struct MaskState {
        Spotlight spotlight;
        Vec2 outputSize;
        Vec2 cursor;
        bool revealEnabled = false;
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    };
    std::unordered_map<std::string, MaskState> maskStates;
    std::unordered_map<std::string, Vec2> fanDirections;
    struct LastWallpaper {
        std::filesystem::path path;
        Profile profile;
    };
    std::unordered_map<std::string, LastWallpaper> lastWallpaperTextures;
    struct ReplacementFade {
        TextureHandle previousTexture;
        std::chrono::steady_clock::time_point started;
    };
    std::unordered_map<std::filesystem::path, ReplacementFade> replacementFades;
    std::unordered_set<std::filesystem::path> reportedImageErrors;
    CHyprSignalListener renderStage;
    CHyprSignalListener workspaceActive;
    CHyprSignalListener workspaceMove;
    CHyprSignalListener monitorAdded;
    CHyprSignalListener monitorRemoved;
    CHyprSignalListener monitorFocused;
    CHyprSignalListener cursorMove;
    SP<SHyprCtlCommand> reloadCommand;
    SP<SHyprCtlCommand> spotlightCommand;
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();

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
                fanDirections.erase(monitor->m_name);
                const auto prefix = monitor->m_name + ":";
                std::erase_if(maskTextures, [&prefix](const auto& item) { return item.first.starts_with(prefix); });
                std::erase_if(maskStates, [&prefix](const auto& item) { return item.first.starts_with(prefix); });
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
            const auto beforePlans = engine.plans();
            const auto previous = engine.activeOutput();
            if (!engine.setCursor({position.x, position.y}))
                return;
            const auto afterPlans = engine.plans();
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

            const auto after = std::ranges::find_if(afterPlans, [&current](const auto& plan) { return plan.output == *current; });
            if (after == afterPlans.end())
                return;
            const auto before = std::ranges::find_if(beforePlans, [&current](const auto& plan) { return plan.output == *current; });
            if (after->transitioning || (before != beforePlans.end() && before->transitioning)) {
                damageOutput(*current);
                return;
            }

            CRegion changedRegion;
            const auto addEffectBounds = [this, &changedRegion](const RenderPlan& plan) {
                if (!plan.maskEnabled || !plan.revealEnabled)
                    return;
                const auto sample = SpotlightSample{
                    .spotlight = plan.profile.spotlight,
                    .outputSize = {plan.logicalBounds.size.x, plan.logicalBounds.size.y},
                    .cursor = plan.cursorLocal,
                    .revealEnabled = true,
                    .fanDirection = fanDirections.contains(plan.output) ? fanDirections.at(plan.output) : Vec2{0.0, 1.0},
                };
                if (const auto bounds = spotlightEffectBounds(sample))
                    changedRegion.add(CBox{
                        plan.logicalBounds.position.x + bounds->position.x,
                        plan.logicalBounds.position.y + bounds->position.y,
                        bounds->size.x,
                        bounds->size.y,
                    });
            };
            if (before != beforePlans.end())
                addEffectBounds(*before);
            addEffectBounds(*after);
            if (!changedRegion.empty())
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
        maskTextures.clear();
        replacementFades.clear();
    }

    void syncOutput(PHLMONITOR monitor) {
        if (!monitor)
            return;
        const auto workspace = normalWorkspace(monitor->m_activeWorkspace);
        (void)engine.upsertOutput({
            .name = monitor->m_name,
            .logicalBounds = {{monitor->m_position.x, monitor->m_position.y}, {monitor->m_size.x, monitor->m_size.y}},
            .workspace = workspace > 0 ? std::optional{workspace} : std::nullopt,
        });
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
            if (monitor && !Fullscreen::controller()->hasFullscreen(monitor) && engine.planFor(monitor->m_name))
                g_pHyprRenderer->damageMonitor(monitor);
    }

    void scheduleManagedFrames() {
        damageManagedOutputs();
    }

    void damageOutput(const std::string& name) {
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && monitor->m_name == name && !Fullscreen::controller()->hasFullscreen(monitor) && engine.planFor(name)) {
                g_pHyprRenderer->damageMonitor(monitor);
                return;
            }
    }

    void applyPendingConfig() {
        std::optional<Error> error;
        if (auto candidate = watcher.takeConfig(error)) {
            config = std::move(*candidate);
            engine.applyConfig(config);
            cache.setBudget(config.textureCacheBytes);
            wallpaperWatcher.setPaths(config);
            maskTextures.clear();
            maskStates.clear();
            const auto paths = wallpaperPaths(config);
            std::erase_if(reportedImageErrors, [&paths](const auto& path) { return !paths.contains(path); });
            damageManagedOutputs();
        }
        if (error) {
            std::lock_guard lock{errorMutex};
            if (!lastConfigError || lastConfigError->message != error->message)
                Log::logger->log(Log::ERR, "Luxaxis config {}: {}", error->source, error->message);
            lastConfigError = std::move(error);
        }
    }

    TextureHandle createMaskTexture(PHLMONITOR monitor, const RenderPlan& plan, const Profile& profile, const bool revealEnabled) {
        constexpr std::uint32_t width = 192;
        constexpr std::uint32_t height = 192;
        // Spotlight units and cursor coordinates are logical. The generated
        // mask is uploaded at the transformed pixel size and stretched by the
        // render pass, so geometry remains scale-independent.
        const auto outputSize = Vec2{monitor->m_size.x, monitor->m_size.y};
        if (!revealEnabled) {
            std::array pixels{
                static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskColor.r, 0.0, 1.0) * 255.0),
                static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskColor.g, 0.0, 1.0) * 255.0),
                static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskColor.b, 0.0, 1.0) * 255.0),
                static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskOpacity, 0.0, 1.0) * 255.0),
            };
            return wrapTexture(g_pHyprRenderer->createTexture(DRM_FORMAT_ABGR8888, pixels.data(), 4U, {1.0, 1.0}, false, false));
        }
        const auto anchor = Vec2{profile.spotlight.anchor.x * outputSize.x, profile.spotlight.anchor.y * outputSize.y};
        const auto previousDirection = fanDirections.contains(monitor->m_name) ? fanDirections.at(monitor->m_name) : Vec2{0.0, 1.0};
        const auto direction = resolveFanDirection(anchor, plan.cursorLocal, previousDirection);
        if (std::hypot(anchor.x - plan.cursorLocal.x, anchor.y - plan.cursorLocal.y) > 1e-6)
            fanDirections[monitor->m_name] = direction;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto point = Vec2{
                    (static_cast<double>(x) + 0.5) * outputSize.x / width,
                    (static_cast<double>(y) + 0.5) * outputSize.y / height,
                };
                const auto alpha = maskAlphaAt(
                    SpotlightSample{
                        .spotlight = profile.spotlight,
                        .outputSize = outputSize,
                        .cursor = plan.cursorLocal,
                        .revealEnabled = revealEnabled,
                        .fanDirection = direction,
                    },
                    point);
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                pixels[offset + 0] = static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskColor.r, 0.0, 1.0) * 255.0);
                pixels[offset + 1] = static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskColor.g, 0.0, 1.0) * 255.0);
                pixels[offset + 2] = static_cast<std::uint8_t>(std::clamp(profile.spotlight.maskColor.b, 0.0, 1.0) * 255.0);
                pixels[offset + 3] = static_cast<std::uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255.0);
            }
        }
        auto texture = g_pHyprRenderer->createTexture(DRM_FORMAT_ABGR8888, pixels.data(), width * 4U, {static_cast<double>(width), static_cast<double>(height)}, false, false);
        return wrapTexture(std::move(texture));
    }

    TextureHandle maskTexture(PHLMONITOR monitor, const RenderPlan& plan, const Profile& profile, const bool revealEnabled, const std::string& key) {
        if (profile.spotlight.type == SpotlightType::None)
            return {};
        const auto cacheKey = monitor->m_name + ":" + key;
        const auto outputSize = Vec2{monitor->m_size.x, monitor->m_size.y};
        const auto cursor = revealEnabled ? plan.cursorLocal : Vec2{};
        const MaskState expected{profile.spotlight, outputSize, cursor, revealEnabled, monitor->m_transform};
        auto& mask = maskTextures[cacheKey];
        const auto state = maskStates.find(cacheKey);
        if (!mask || state == maskStates.end() || state->second.spotlight != expected.spotlight || state->second.outputSize != expected.outputSize ||
            state->second.cursor != expected.cursor || state->second.revealEnabled != expected.revealEnabled || state->second.transform != expected.transform) {
            mask = createMaskTexture(monitor, plan, profile, revealEnabled);
            maskStates.insert_or_assign(cacheKey, expected);
        }
        return mask;
    }

    TextureHandle wallpaperTexture(const std::vector<std::filesystem::path>& candidates, std::filesystem::path* selectedPath = nullptr) {
        for (const auto& candidate : candidates) {
            const auto snapshot = cache.snapshot(candidate);
            if (!snapshot.error.empty()) {
                if (reportedImageErrors.insert(candidate).second)
                    Log::logger->log(Log::ERR, "Luxaxis wallpaper {}: {}", candidate.string(), snapshot.error);
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
            const auto texture = g_pHyprRenderer->createTexture(
                DRM_FORMAT_ABGR8888,
                request.image.rgba.data(),
                request.image.stride,
                {static_cast<double>(request.image.width), static_cast<double>(request.image.height)},
                false,
                false);
            if (!texture) {
                (void)cache.failUpload(request, "Hyprland rejected the wallpaper texture upload");
                continue;
            }
            const auto bytes = static_cast<std::size_t>(request.image.stride) * request.image.height;
            if (cache.completeUpload(request, wrapTexture(texture), bytes) && request.previousTexture)
                replacementFades.insert_or_assign(request.path, ReplacementFade{request.previousTexture, now});
        }
        if (cache.hasPendingUploads())
            damageManagedOutputs();
    }

    CRegion transitionClip(const Transition& transition, const double progress, const Vec2 origin, const Vector2D logicalSize, const float scale) {
        CRegion region;
        const auto eased = easeProgress(progress, transition.easing);
        const auto addLogicalBox = [&region, scale](const double x, const double y, const double width, const double height) {
            if (width > 0.0 && height > 0.0)
                region.add(CBox{x * scale, y * scale, width * scale, height * scale});
        };
        if (eased <= 0.0)
            return region;
        if (eased >= 1.0) {
            addLogicalBox(0.0, 0.0, logicalSize.x, logicalSize.y);
            return region;
        }
        if (transition.type == TransitionType::Wipe) {
            const auto fromRight = origin.x > logicalSize.x * 0.5;
            const auto edge = fromRight ? logicalSize.x * (1.0 - eased) : logicalSize.x * eased;
            addLogicalBox(fromRight ? edge : 0.0, 0.0, fromRight ? logicalSize.x - edge : edge, logicalSize.y);
            return region;
        }
        if (transition.type == TransitionType::Outer) {
            const auto left = origin.x * eased;
            const auto right = origin.x + (logicalSize.x - origin.x) * (1.0 - eased);
            const auto top = origin.y * eased;
            const auto bottom = origin.y + (logicalSize.y - origin.y) * (1.0 - eased);
            addLogicalBox(0.0, 0.0, logicalSize.x, top);
            addLogicalBox(0.0, bottom, logicalSize.x, logicalSize.y - bottom);
            addLogicalBox(0.0, top, left, bottom - top);
            addLogicalBox(right, top, logicalSize.x - right, bottom - top);
            return region;
        }

        constexpr int GRID = 48;
        for (int y = 0; y < GRID; ++y) {
            for (int x = 0; x < GRID; ++x) {
                const Vec2 point{
                    (static_cast<double>(x) + 0.5) * logicalSize.x / GRID,
                    (static_cast<double>(y) + 0.5) * logicalSize.y / GRID,
                };
                if (transitionReveal(transition, progress, point, {logicalSize.x, logicalSize.y}, origin) < 0.5)
                    continue;
                region.add(CBox{
                    (point.x - logicalSize.x / GRID * 0.5) * scale,
                    (point.y - logicalSize.y / GRID * 0.5) * scale,
                    logicalSize.x / GRID * scale + 1.0,
                    logicalSize.y / GRID * scale + 1.0,
                });
            }
        }
        return region;
    }

    void drawWallpaperTexture(PHLMONITOR monitor, const SP<Render::ITexture>& texture, const Profile& profile, const float alpha = 1.F, const CRegion& clip = {}) {
        if (!texture)
            return;
        const auto outputSize = monitor->m_transformedSize;
        const auto imageSize = Vector2D{texture->m_size.x, texture->m_size.y};
        const auto fit = profile.fit;
        const double scaleX = outputSize.x / imageSize.x;
        const double scaleY = outputSize.y / imageSize.y;
        const double scale = fit == FitMode::Stretch ? 1.0 : (fit == FitMode::Contain ? std::min(scaleX, scaleY) : std::max(scaleX, scaleY));
        const auto imageBox = fit == FitMode::Stretch ? outputSize : Vector2D{imageSize.x * scale, imageSize.y * scale};
        const auto excess = outputSize - imageBox;
        const auto origin = Vector2D{excess.x * profile.position.x, excess.y * profile.position.y};
        CTexPassElement::SRenderData data{};
        data.tex = texture;
        data.box = {origin, imageBox};
        data.a = alpha;
        data.clipRegion = clip;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
    }

    void renderWallpaper() {
        const auto now = std::chrono::steady_clock::now();
        const auto frameDelta = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrame);
        lastFrame = now;
        engine.advance(frameDelta);
        applyPendingConfig();
        auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor)
            return;
        uploadPendingTextures();
        syncOutput(monitor);
        const auto plan = engine.planFor(monitor->m_name);
        if (!plan)
            return;

        for (auto iterator = replacementFades.begin(); iterator != replacementFades.end();) {
            if (now - iterator->second.started >= 120ms)
                iterator = replacementFades.erase(iterator);
            else
                ++iterator;
        }

        std::set<std::filesystem::path> pinned;
        const auto plans = engine.plans();
        for (const auto& other : plans)
            for (const auto& candidate : other.wallpaperCandidates)
                pinned.insert(candidate);
        for (const auto& other : plans) {
            if (other.previousProfile)
                pinned.insert(other.previousProfile->wallpaper);
        }
        for (const auto& [unused, last] : lastWallpaperTextures) {
            (void)unused;
            pinned.insert(last.path);
        }
        cache.setPinned(pinned);

        // During a transition, only the destination image may replace the
        // source. A default-profile fallback is allowed once the transition
        // has finished, but must not make a missing destination flash early.
        TextureHandle wallpaper;
        std::filesystem::path selectedWallpaperPath;
        const auto destinationSnapshot = cache.snapshot(plan->profile.wallpaper);
        if (plan->transitioning) {
            wallpaper = wallpaperTexture({plan->profile.wallpaper}, &selectedWallpaperPath);
        } else {
            wallpaper = wallpaperTexture({plan->profile.wallpaper}, &selectedWallpaperPath);
            if (!wallpaper && destinationSnapshot.status == ImageStatus::Failed)
                wallpaper = wallpaperTexture(
                    plan->wallpaperCandidates.size() > 1 ? std::vector{plan->wallpaperCandidates[1]} : std::vector<std::filesystem::path>{},
                    &selectedWallpaperPath);
        }

        const CBox outputBox{{0.0, 0.0}, monitor->m_transformedSize};
        const auto currentTexture = unwrapTexture(wallpaper);
        std::optional<float> replacementProgress;
        SP<Render::ITexture> replacementTexture;
        if (!plan->transitioning && !selectedWallpaperPath.empty()) {
            if (const auto replacement = replacementFades.find(selectedWallpaperPath); replacement != replacementFades.end()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - replacement->second.started);
                const auto progress = std::clamp(static_cast<float>(elapsed.count()) / 120.F, 0.F, 1.F);
                if (progress < 1.F) {
                    replacementProgress = progress;
                    replacementTexture = unwrapTexture(replacement->second.previousTexture);
                } else {
                    replacementFades.erase(replacement);
                }
            }
        }
        CRectPassElement::SRectData baseData{};
        baseData.box = outputBox;
        baseData.color = hyprColor(plan->fallbackColor);
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(std::move(baseData)));
        if (plan->transitioning && plan->previousProfile) {
            const auto oldTexture = unwrapTexture(wallpaperTexture({plan->previousProfile->wallpaper}));
            const auto sameWallpaper = plan->previousProfile->wallpaper == plan->profile.wallpaper;
            if (sameWallpaper) {
                if (plan->interruptedSourceProfile) {
                    const auto sourceTexture = unwrapTexture(wallpaperTexture({plan->interruptedSourceProfile->wallpaper}));
                    if (sourceTexture)
                        drawWallpaperTexture(monitor, sourceTexture, *plan->interruptedSourceProfile);
                    if (oldTexture) {
                        if (plan->interruptedTransition.type == TransitionType::Fade)
                            drawWallpaperTexture(
                                monitor, oldTexture, *plan->previousProfile, static_cast<float>(plan->interruptedProgress));
                        else
                            drawWallpaperTexture(
                                monitor,
                                oldTexture,
                                *plan->previousProfile,
                                1.F,
                                transitionClip(
                                    plan->interruptedTransition,
                                    plan->interruptedProgress,
                                    plan->interruptedOrigin,
                                    monitor->m_size,
                                    monitor->m_scale));
                    }
                } else if (currentTexture) {
                    drawWallpaperTexture(monitor, currentTexture, plan->profile);
                } else if (oldTexture) {
                    drawWallpaperTexture(monitor, oldTexture, *plan->previousProfile);
                }
            } else {
                if (plan->interruptedSourceProfile) {
                    const auto sourceTexture = unwrapTexture(wallpaperTexture({plan->interruptedSourceProfile->wallpaper}));
                    if (sourceTexture)
                        drawWallpaperTexture(monitor, sourceTexture, *plan->interruptedSourceProfile);
                    if (oldTexture) {
                        if (plan->interruptedTransition.type == TransitionType::Fade)
                            drawWallpaperTexture(
                                monitor, oldTexture, *plan->previousProfile, static_cast<float>(plan->interruptedProgress));
                        else
                            drawWallpaperTexture(
                                monitor,
                                oldTexture,
                                *plan->previousProfile,
                                1.F,
                                transitionClip(
                                    plan->interruptedTransition,
                                    plan->interruptedProgress,
                                    plan->interruptedOrigin,
                                    monitor->m_size,
                                    monitor->m_scale));
                    }
                } else if (oldTexture) {
                    drawWallpaperTexture(monitor, oldTexture, *plan->previousProfile);
                }
                if (currentTexture) {
                    if (plan->transition.type == TransitionType::Fade)
                        drawWallpaperTexture(monitor, currentTexture, plan->profile, static_cast<float>(plan->transitionProgress));
                    else
                        drawWallpaperTexture(
                            monitor,
                            currentTexture,
                            plan->profile,
                            1.F,
                            transitionClip(plan->transition, plan->transitionProgress, plan->transitionOrigin, monitor->m_size, monitor->m_scale));
                }
            }
            if (!currentTexture && oldTexture)
                lastWallpaperTextures[monitor->m_name] = LastWallpaper{plan->previousProfile->wallpaper, *plan->previousProfile};
        } else if (currentTexture) {
            lastWallpaperTextures[monitor->m_name] = LastWallpaper{selectedWallpaperPath, plan->profile};
            if (replacementProgress && replacementTexture) {
                drawWallpaperTexture(monitor, replacementTexture, plan->profile, 1.F - *replacementProgress);
                drawWallpaperTexture(monitor, currentTexture, plan->profile, *replacementProgress);
            } else {
                drawWallpaperTexture(monitor, currentTexture, plan->profile);
            }
        } else if (destinationSnapshot.status != ImageStatus::Failed) {
            const auto previous = lastWallpaperTextures.find(monitor->m_name);
            if (previous != lastWallpaperTextures.end()) {
                if (const auto texture = unwrapTexture(wallpaperTexture({previous->second.path})))
                    drawWallpaperTexture(monitor, texture, previous->second.profile);
            }
        }

        if (plan->maskEnabled) {
            auto drawMask = [&](const Profile& profile, const float alpha, const std::string& key) {
                if (auto texture = unwrapTexture(maskTexture(monitor, *plan, profile, plan->revealEnabled, key))) {
                CTexPassElement::SRenderData data{};
                data.tex = std::move(texture);
                data.box = outputBox;
                data.a = alpha;
                g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
                }
            };
            if (plan->transitioning && plan->previousProfile) {
                const auto& destinationSpotlight = plan->profile.spotlight;
                const auto& previousSpotlight = plan->previousProfile->spotlight;
                if (plan->interruptedSourceProfile) {
                    const auto& interruptedSpotlight = plan->interruptedSourceProfile->spotlight;
                    const auto compositeWeight = static_cast<float>(1.0 - plan->transitionProgress);
                    if (interruptedSpotlight.type != SpotlightType::None && interruptedSpotlight.type == previousSpotlight.type) {
                        auto interpolated = *plan->previousProfile;
                        interpolated.spotlight = interpolateSpotlight(interruptedSpotlight, previousSpotlight, plan->interruptedProgress);
                        drawMask(interpolated, compositeWeight, "interrupted-interpolated");
                    } else {
                        drawMask(*plan->interruptedSourceProfile, compositeWeight * static_cast<float>(1.0 - plan->interruptedProgress), "interrupted-old");
                        drawMask(*plan->previousProfile, compositeWeight * static_cast<float>(plan->interruptedProgress), "interrupted-new");
                    }
                    drawMask(plan->profile, static_cast<float>(plan->transitionProgress), "new");
                } else if (previousSpotlight.type != SpotlightType::None && previousSpotlight.type == destinationSpotlight.type) {
                    auto interpolated = plan->profile;
                    interpolated.spotlight = interpolateSpotlight(previousSpotlight, destinationSpotlight, plan->transitionProgress);
                    drawMask(interpolated, 1.F, "interpolated");
                } else {
                    drawMask(*plan->previousProfile, static_cast<float>(1.0 - plan->transitionProgress), "old");
                    drawMask(plan->profile, static_cast<float>(plan->transitionProgress), "new");
                }
            } else {
                drawMask(plan->profile, 1.F, "current");
            }
        }
        if ((plan->transitioning || replacementProgress) && !Fullscreen::controller()->hasFullscreen(monitor))
            g_pHyprRenderer->damageMonitor(monitor);
    }
};

Adapter::Adapter(HANDLE handle, std::filesystem::path configPath, std::filesystem::path home) {
    auto loaded = loadConfig(configPath, home);
    auto config = loaded ? std::move(loaded.value()) : fallbackConfig();
    if (!loaded)
        Log::logger->log(Log::ERR, "Luxaxis config {}: {}; using fallback color until a valid config is loaded", loaded.error().source, loaded.error().message);
    impl_ = std::make_unique<Impl>(handle, std::move(configPath), std::move(home), std::move(config));
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

void Adapter::unregisterCommands() {
    if (impl_->reloadCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(impl_->handle, impl_->reloadCommand);
        impl_->reloadCommand.reset();
    }
    if (impl_->spotlightCommand) {
        HyprlandAPI::unregisterHyprCtlCommand(impl_->handle, impl_->spotlightCommand);
        impl_->spotlightCommand.reset();
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

} // namespace luxaxis::hyprland
