// A real plugin shared library, dynamically loaded by the PluginHost::Load test.
// It registers a subsystem on load and removes it on unload; the subsystem bumps
// a counter the test observes across the library boundary via DraconicTestPluginTicks.

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.runtime;

using namespace draconic::foundation;
using namespace draconic::runtime;

#if defined(_WIN32)
#define DRACONIC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define DRACONIC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
    int g_ticks = 0;

    class PluginSubsystem final : public Subsystem
    {
    public:
        void Update(f32) override { ++g_ticks; }
    };

    class TestPlugin final : public IRuntimePlugin
    {
    public:
        [[nodiscard]] StringView Name() const noexcept override { return u8"DraconicTestPlugin"; }

        // The plugin owns its subsystem; it registers it non-owningly and removes
        // it on unload, before the host closes this library.
        void OnLoad(Context& context) override
        {
            context.RegisterSubsystem<PluginSubsystem>(&m_subsystem);
        }
        void OnUnload(Context& context) override { context.RemoveSubsystem<PluginSubsystem>(); }

    private:
        PluginSubsystem m_subsystem;
    };
}

// Factory resolved by PluginHost::Load. The instance is owned by this library.
extern "C" DRACONIC_PLUGIN_EXPORT IRuntimePlugin* DraconicCreatePlugin()
{
    static TestPlugin plugin;
    return &plugin;
}

// Lets the test observe the plugin subsystem's activity across the library boundary.
extern "C" DRACONIC_PLUGIN_EXPORT int DraconicTestPluginTicks() { return g_ticks; }
