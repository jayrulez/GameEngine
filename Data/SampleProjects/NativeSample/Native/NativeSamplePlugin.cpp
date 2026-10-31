// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The reference native game plugin. Its one registration entry is OnLoad(SubsystemManager&):
// components, subsystems and script facades all register explicitly there (never through static
// initializers), and OnUnload reverses it. The exported
// CreatePlugin below is the whole contract: PluginHost dlopens it in dev builds, and the
// ship player's checked-in stub references it statically.

#include "Core/Prelude.h"
#include "Core/Log/Log.h"

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
    class NativeSamplePlugin final : public IRuntimePlugin
    {
    public:
        [[nodiscard]] StringView Name() const noexcept override { return u8"NativeSample"; }

        void OnLoad(SubsystemManager&) override
        {
            // Registration point: subsystems.RegisterSubsystem<...>, script facades,
            // component managers: all explicit, all reversed in OnUnload.
            LOG_INFO(u8"Game", u8"NativeSample plugin loaded");
        }
        void OnUnload(SubsystemManager&) override {}
    };
}

extern "C" PLUGIN_EXPORT IRuntimePlugin* CreatePlugin()
{
    static NativeSamplePlugin plugin;
    return &plugin;
}
