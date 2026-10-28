// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler - `foundation.profiler` (Specs/profiler.md): the capture store every thread
/// records into (:store), the sources a viewer reads it through (:source: the live store or a
/// capture), the stream a capture file is (:stream), a viewer's own copy of frames (:recording), and
/// what a viewer shows computed from a source, nothing drawn (:window).

export module foundation.profiler;

export import :store;
export import :source;
export import :stream;
export import :recording;
export import :window;
