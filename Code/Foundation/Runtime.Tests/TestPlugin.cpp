// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A real plugin shared library, dynamically loaded by the PluginHost::Load test.
// It registers a subsystem on load and removes it on unload; the subsystem bumps
// a counter the test observes across the library boundary via TestPluginTicks.

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;

using namespace foundation::core;
using namespace foundation::runtime;

#if defined(_WIN32)
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
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
        [[nodiscard]] StringView Name() const noexcept override { return u8"TestPlugin"; }

        // The plugin owns its subsystem; it registers it non-owningly and removes
        // it on unload, before the host closes this library.
        void OnLoad(SubsystemManager& context) override
        {
            context.RegisterSubsystem<PluginSubsystem>(&m_subsystem);
        }
        void OnUnload(SubsystemManager& context) override { context.RemoveSubsystem<PluginSubsystem>(); }

    private:
        PluginSubsystem m_subsystem;
    };
}

// Factory resolved by PluginHost::Load. The instance is owned by this library.
extern "C" PLUGIN_EXPORT IRuntimePlugin* CreatePlugin()
{
    static TestPlugin plugin;
    return &plugin;
}

// Lets the test observe the plugin subsystem's activity across the library boundary.
extern "C" PLUGIN_EXPORT int TestPluginTicks() { return g_ticks; }
