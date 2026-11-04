// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A fixture plugin part: its load and unload count through a C symbol Runtime.Tests reads, to
// show the generated registry and the host reach the part through its manifest-named entry.

#include "Core/Prelude.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;

namespace
{
    int g_loads = 0;

    void Load(foundation::runtime::PluginContext&) { ++g_loads; }
    void Unload(foundation::runtime::PluginContext&) { --g_loads; }
}

extern "C" int ProbeEngineLoads() { return g_loads; }

PLUGIN_PART(&Load, &Unload)
