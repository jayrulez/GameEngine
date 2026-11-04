// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime.Client - :client_services partition.
//
// What a windowed application offers the plugins its process loads: the application host it
// runs on (the graphics device, the shell, the subsystems) and the data mount engine data is read
// through. A plugin finds it by type (PluginContext::Find<ClientServices>), and finds none in a
// process with no window (a cook, an export), so it leaves out what needs one.

module;
#include "Core/Prelude.h"

export module foundation.runtime.client:client_services;

import foundation.vfs;
import :app;

export namespace foundation::runtime
{
    struct ClientServices
    {
        IApplicationHost* host = nullptr;               // the host the application runs on
        foundation::vfs::IFileSystem* data = nullptr;   // the data mount ("Engine/Shaders/...")
    };
}
