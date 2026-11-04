// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Imgui.Engine's entry. Its load adds the ImguiSubsystem to the process's subsystems, from the
// client services a windowed application offers (its graphics device and data mount); a process
// with no window or no device offers none, and nothing is added. The application still drives it
// each frame (NewFrame, then its widgets).

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client; // ClientServices, IApplicationHost
import foundation.graphics;       // GraphicsDevice
import imgui.engine;              // ImguiSubsystem

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        const foundation::runtime::ClientServices* client = context.Find<foundation::runtime::ClientServices>();
        if (client == nullptr || client->host == nullptr || client->data == nullptr)
        {
            return;
        }
        foundation::graphics::GraphicsDevice* graphics = client->host->Graphics();
        if (graphics == nullptr || graphics->Raw() == nullptr)
        {
            return;
        }
        context.Subsystems().AddSubsystem<imgui::ImguiSubsystem>(*graphics->Raw(), graphics->FramesInFlight(),
                                                                *client->data);
    }

    void Unload(foundation::runtime::PluginContext& context)
    {
        context.Subsystems().RemoveSubsystem<imgui::ImguiSubsystem>();
    }
}

PLUGIN_PART(&Load, &Unload)
