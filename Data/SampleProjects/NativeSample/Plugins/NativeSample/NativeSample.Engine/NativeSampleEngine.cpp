// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// NativeSample.Engine - the reference game's native code.
//
// Load is the one registration point: subsystems, scene managers, script facades and types all
// register through the context here (never through static initializers), and what the context
// records is reversed when the plugin unloads (a reload, the project closing). Unload removes what
// the recording cannot, such as a subsystem Load added.

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;

namespace
{
    void Load(foundation::runtime::PluginContext&)
    {
        LOG_INFO(u8"Game", u8"NativeSample loaded");
    }
    void Unload(foundation::runtime::PluginContext&) {}
}

PLUGIN_PART(&Load, &Unload)
