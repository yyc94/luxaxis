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
#include <src/output/Monitor.hpp>
#include <src/pointer/PointerManager.hpp>
#include <src/render/Renderer.hpp>
#include <src/render/pass/ClearPassElement.hpp>
#include <src/render/pass/RectPassElement.hpp>
#include <src/render/pass/TexPassElement.hpp>
#include <src/state/MonitorState.hpp>

#include <hyprgraphics/image/Image.hpp>
#include <drm_fourcc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>

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
    const auto width = static_cast<std::uint32_t>(size.x);
    const auto height = static_cast<std::uint32_t>(size.y);
    const auto sourceStride = surface->stride();
    if (width == 0 || height == 0 || sourceStride < static_cast<int>(width * 4U))
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
    ConfigWatcher(std::filesystem::path source, std::filesystem::path home) : source_(std::move(source)), home_(std::move(home)) {
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
        while (!stopping_) {
            std::error_code ec;
            const auto current = std::filesystem::last_write_time(source_, ec);
            const bool changed = forceReload_.exchange(false) || (!ec && (!observed || current != *observed));
            if (changed) {
                observed = ec ? std::optional<std::filesystem::file_time_type>{} : std::optional{current};
                auto loaded = loadConfig(source_, home_);
                std::lock_guard lock{mutex_};
                if (loaded)
                    pending_ = std::move(loaded.value());
                else
                    error_ = loaded.error();
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
};

} // namespace

struct Adapter::Impl {
    HANDLE handle;
    std::filesystem::path configPath;
    std::filesystem::path home;
    Config config;
    Engine engine;
    ImageCache cache;
    ConfigWatcher watcher;
    std::mutex errorMutex;
    std::optional<Error> lastConfigError;
    std::unordered_map<std::string, TextureHandle> maskTextures;
    std::unordered_map<std::string, std::uint64_t> maskRevisions;
    std::unordered_map<std::string, SP<Render::ITexture>> lastWallpaperTextures;
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
          cache(config.textureCacheBytes, decodeImage), watcher(configPath, home) {
        syncOutputs();

        renderStage = Event::bus()->m_events.render.stage.listen([this](eRenderStage stage) {
            if (stage == RENDER_POST_WALLPAPER)
                renderWallpaper();
        });
        workspaceActive = Event::bus()->m_events.workspace.active.listen([this](PHLWORKSPACE workspace) {
            if (workspace) {
                if (auto monitor = workspace->m_monitor.lock())
                    syncOutput(monitor);
            }
            damageManagedOutputs();
        });
        workspaceMove = Event::bus()->m_events.workspace.moveToMonitor.listen([this](PHLWORKSPACE workspace, PHLMONITOR monitor) {
            if (workspace)
                syncOutput(monitor);
            damageManagedOutputs();
        });
        monitorAdded = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR monitor) {
            syncOutput(monitor);
            damageManagedOutputs();
        });
        monitorRemoved = Event::bus()->m_events.monitor.removed.listen([this](PHLMONITOR monitor) {
            if (monitor) {
                (void)engine.removeOutput(monitor->m_name);
                lastWallpaperTextures.erase(monitor->m_name);
            }
            damageManagedOutputs();
        });
        monitorFocused = Event::bus()->m_events.monitor.focused.listen([this](PHLMONITOR monitor) {
            (void)engine.setFocusedOutput(monitor ? std::optional{monitor->m_name} : std::nullopt);
            damageManagedOutputs();
        });
        cursorMove = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D position, Event::SCallbackInfo&) {
            (void)engine.setCursor({position.x, position.y});
            damageManagedOutputs();
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
    }

    void syncOutput(PHLMONITOR monitor) {
        if (!monitor)
            return;
        const auto workspace = normalWorkspace(monitor->m_activeWorkspace);
        (void)engine.upsertOutput({
            .name = monitor->m_name,
            .logicalBounds = {{monitor->m_position.x, monitor->m_position.y}, {monitor->m_transformedSize.x, monitor->m_transformedSize.y}},
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
            if (monitor && engine.planFor(monitor->m_name))
                g_pHyprRenderer->damageMonitor(monitor);
    }

    void applyPendingConfig() {
        std::optional<Error> error;
        if (auto candidate = watcher.takeConfig(error)) {
            config = std::move(*candidate);
            engine.applyConfig(config);
            cache.setBudget(config.textureCacheBytes);
            maskTextures.clear();
            maskRevisions.clear();
            damageManagedOutputs();
        }
        if (error) {
            std::lock_guard lock{errorMutex};
            if (!lastConfigError || lastConfigError->message != error->message)
                Log::logger->log(Log::ERR, "Luxaxis config {}: {}", error->source, error->message);
            lastConfigError = std::move(error);
        }
    }

    TextureHandle createMaskTexture(PHLMONITOR monitor, const RenderPlan& plan) {
        constexpr std::uint32_t width = 192;
        constexpr std::uint32_t height = 192;
        const auto outputSize = Vec2{monitor->m_transformedSize.x, monitor->m_transformedSize.y};
        const auto direction = resolveFanDirection(
            {plan.profile.spotlight.anchor.x * outputSize.x, plan.profile.spotlight.anchor.y * outputSize.y}, plan.cursorLocal, {0.0, 1.0});
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto point = Vec2{
                    (static_cast<double>(x) + 0.5) * outputSize.x / width,
                    (static_cast<double>(y) + 0.5) * outputSize.y / height,
                };
                const auto alpha = maskAlphaAt(
                    SpotlightSample{
                        .spotlight = plan.profile.spotlight,
                        .outputSize = outputSize,
                        .cursor = plan.cursorLocal,
                        .revealEnabled = plan.revealEnabled,
                        .fanDirection = direction,
                    },
                    point);
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                pixels[offset + 0] = static_cast<std::uint8_t>(std::clamp(plan.profile.spotlight.maskColor.r, 0.0, 1.0) * 255.0);
                pixels[offset + 1] = static_cast<std::uint8_t>(std::clamp(plan.profile.spotlight.maskColor.g, 0.0, 1.0) * 255.0);
                pixels[offset + 2] = static_cast<std::uint8_t>(std::clamp(plan.profile.spotlight.maskColor.b, 0.0, 1.0) * 255.0);
                pixels[offset + 3] = static_cast<std::uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255.0);
            }
        }
        auto texture = g_pHyprRenderer->createTexture(DRM_FORMAT_ABGR8888, pixels.data(), width * 4U, {static_cast<double>(width), static_cast<double>(height)}, false, false);
        return wrapTexture(std::move(texture));
    }

    TextureHandle wallpaperTexture(const std::vector<std::filesystem::path>& candidates) {
        for (const auto& candidate : candidates) {
            if (auto current = cache.texture(candidate))
                return current;
            (void)cache.request(candidate);
        }
        return {};
    }

    void uploadPendingTextures() {
        for (auto& request : cache.takeUploads()) {
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
            (void)cache.completeUpload(request, wrapTexture(texture), bytes);
        }
    }

    CRegion transitionClip(const RenderPlan& plan, const Vector2D outputSize) {
        CRegion region;
        constexpr int GRID = 48;
        const auto progress = plan.transitionProgress;
        for (int y = 0; y < GRID; ++y) {
            for (int x = 0; x < GRID; ++x) {
                const Vec2 point{
                    (static_cast<double>(x) + 0.5) * outputSize.x / GRID,
                    (static_cast<double>(y) + 0.5) * outputSize.y / GRID,
                };
                if (transitionReveal(plan.transition, progress, point, {outputSize.x, outputSize.y}, plan.transitionOrigin) < 0.5)
                    continue;
                region.add(CBox{
                    point.x - outputSize.x / GRID * 0.5,
                    point.y - outputSize.y / GRID * 0.5,
                    outputSize.x / GRID + 1.0,
                    outputSize.y / GRID + 1.0,
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

        std::set<std::filesystem::path> pinned{plan->profile.wallpaper};
        if (plan->previousProfile)
            pinned.insert(plan->previousProfile->wallpaper);
        cache.setPinned(pinned);

        // During a transition, only the destination image may replace the
        // source. A default-profile fallback is allowed once the transition
        // has finished, but must not make a missing destination flash early.
        const auto wallpaper = plan->transitioning ? wallpaperTexture({plan->profile.wallpaper}) : wallpaperTexture(plan->wallpaperCandidates);

        const CBox outputBox{{0.0, 0.0}, monitor->m_transformedSize};
        const auto currentTexture = unwrapTexture(wallpaper);
        if (plan->transitioning && plan->previousProfile) {
            const auto oldTexture = unwrapTexture(wallpaperTexture({plan->previousProfile->wallpaper}));
            if (oldTexture)
                drawWallpaperTexture(monitor, oldTexture, *plan->previousProfile);
            if (currentTexture) {
                if (plan->transition.type == TransitionType::Fade)
                    drawWallpaperTexture(monitor, currentTexture, plan->profile, static_cast<float>(plan->transitionProgress));
                else
                    drawWallpaperTexture(monitor, currentTexture, plan->profile, 1.F, transitionClip(*plan, monitor->m_transformedSize));
            }
            if (!currentTexture && oldTexture)
                lastWallpaperTextures[monitor->m_name] = oldTexture;
        } else if (currentTexture) {
            lastWallpaperTextures[monitor->m_name] = currentTexture;
            drawWallpaperTexture(monitor, currentTexture, plan->profile);
        } else if (const auto previous = lastWallpaperTextures.find(monitor->m_name); previous != lastWallpaperTextures.end()) {
            drawWallpaperTexture(monitor, previous->second, plan->profile);
        } else {
            CRectPassElement::SRectData data{};
            data.box = outputBox;
            data.color = hyprColor(plan->fallbackColor);
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(std::move(data)));
        }

        if (plan->maskEnabled) {
            const auto keyRevision = plan->revision ^ static_cast<std::uint64_t>(std::llround(plan->cursorLocal.x * 16.0)) ^
                (static_cast<std::uint64_t>(std::llround(plan->cursorLocal.y * 16.0)) << 32U);
            auto& mask = maskTextures[monitor->m_name];
            if (!mask || maskRevisions[monitor->m_name] != keyRevision) {
                mask = createMaskTexture(monitor, *plan);
                maskRevisions[monitor->m_name] = keyRevision;
            }
            if (auto texture = unwrapTexture(mask)) {
                CTexPassElement::SRenderData data{};
                data.tex = std::move(texture);
                data.box = outputBox;
                g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
            }
        }
        if (plan->transitioning)
            damageManagedOutputs();
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
