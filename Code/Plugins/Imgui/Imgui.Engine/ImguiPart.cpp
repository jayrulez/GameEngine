// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Imgui.Engine's entry. Its load registers nothing: the ImguiSubsystem needs the app's graphics
// device and data mount, which a plugin's context does not offer, so each app that uses Imgui
// adds the subsystem itself.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;

namespace
{
    void Load(foundation::runtime::PluginContext&) {}
}

PLUGIN_PART(&Load, nullptr)
