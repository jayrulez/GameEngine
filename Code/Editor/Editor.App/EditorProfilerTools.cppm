// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_profiler_tools partition.
//
// profiler_capture: captures the next few seconds of the running editor (its pages and any
// Play-in-editor game, in one process), writes the capture as files and answers with a summary an
// agent can act on. Only the editor host serves it: a capture needs frames to run, and the
// headless host runs none while a call waits.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_profiler_tools;

import foundation.core;
import foundation.mcp;
import foundation.profiler;
import foundation.profiler.capture;

using namespace foundation::core;

export namespace editor::app
{
    /// What the profiler tool drives: the process's profiler and capture recorder (a test's own),
    /// and the folder captures go in.
    struct ProfilerToolSeams
    {
        foundation::profiler::Profiler* profiler = nullptr;
        foundation::profiler::CaptureRecorder* recorder = nullptr;
        Function<String()> folder;
    };

    /// The number of tools RegisterProfilerTools registers (profiler_capture); a tripwire like
    /// kWindowToolCount.
    inline constexpr usize kProfilerToolCount = 1;

    void RegisterProfilerTools(foundation::mcp::McpServer& server, ProfilerToolSeams seams);
}
