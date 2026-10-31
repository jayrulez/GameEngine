// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.GUI - `foundation.profiler.gui`: the profiler's views on the GUI. They all
/// read through one focus (:focus), which holds the source, the chosen frames and name, and the
/// marked span. The partitions are the views' shared base (:widget); the frame strip and the
/// counter plots on one frame axis (:frame_strip); the chosen frames on one time axis
/// (:timeline); their scopes as a call tree or a list (:scope_table); the chosen name on its own
/// (:scope_detail); the in-game overlay (:overlay); and the views' style sheet (:sheet).

export module foundation.profiler.gui;

export import :focus;
export import :widget;
export import :frame_strip;
export import :timeline;
export import :scope_table;
export import :scope_detail;
export import :overlay;
export import :sheet;

export namespace foundation::profiler::gui
{
    /// Registers, once, the views markup can place by name (FrameStrip, CounterPlot,
    /// ProfileTimeline, ScopeTable, ScopeDetail).
    void RegisterProfilerElements();
}
