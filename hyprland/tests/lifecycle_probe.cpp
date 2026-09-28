#include <src/Compositor.hpp>
#include <src/debug/HyprCtl.hpp>
#include <src/event/EventBus.hpp>
#include <src/plugins/PluginSystem.hpp>
#include <aquamarine/backend/Backend.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

// Stop before GPU startup, using the packaged host's real globals and loader.
bool Aquamarine::CBackend::start() {
    try {
        const auto path = std::getenv("LUXAXIS_TEST_PLUGIN");
        if (!path)
            throw std::runtime_error("missing LUXAXIS_TEST_PLUGIN");
        g_pHyprCtl = makeUnique<CHyprCtl>();
        g_pPluginSystem = makeUnique<CPluginSystem>();
        for (int i = 0; i < 3; ++i) {
            CPlugin* plugin = nullptr;
            std::string error;
            g_pPluginSystem->loadPlugin(path)->then([&](SP<CPromiseResult<CPlugin*>> result) {
                if (result->hasError())
                    error = result->error();
                else
                    plugin = result->result();
            });
            if (!plugin)
                throw std::runtime_error("load failed: " + error);
            if (plugin->m_registeredHyprctlCommands.size() != 2)
                throw std::runtime_error("plugin did not register both commands");
            for (const auto& weak : plugin->m_registeredHyprctlCommands) {
                const auto command = weak.lock();
                if (!command)
                    throw std::runtime_error("registered command expired");
                if (command->name == "luxaxis:reload") {
                    if (command->fn(FORMAT_NORMAL, "") != "reload queued")
                        throw std::runtime_error("reload command failed");
                } else if (command->name == "luxaxis:spotlight") {
                    if (command->fn(FORMAT_NORMAL, "off") != "off" || command->fn(FORMAT_NORMAL, "on") != "on")
                        throw std::runtime_error("Spotlight commands failed");
                }
            }
            Event::bus()->m_events.workspace.active.emit(nullptr);
            g_pPluginSystem->unloadPlugin(plugin, false);
            if (g_pPluginSystem->pluginCount() != 0)
                throw std::runtime_error("plugin remained registered after unload");
            std::fprintf(stderr, "load/unload %d passed\n", i + 1);
        }
        // The compositor has intentionally not completed its own startup.
        std::_Exit(0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Luxaxis lifecycle test failed: %s\n", error.what());
        std::_Exit(1);
    }
}
