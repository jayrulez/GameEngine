// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.GUI - `foundation.profiler.gui` (X7 of Specs/editor-gui.md): the profiler's
/// views on the GUI, read through one focus (:focus: the source, the frames and the name chosen,
/// the span marked); their shared base (:widget); the frame strip and the counters' plots on one
/// frame axis (:frame_strip); the frames chosen on one time axis (:timeline); their scopes as a
/// call tree or a list (:scope_table); the name chosen apart (:scope_detail); the in-game overlay (:overlay); their sheet (:sheet).

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
    /// The views markup places by name (FrameStrip, CounterPlot, ProfileTimeline, ScopeTable,
    /// ScopeDetail), registered once.
    void RegisterProfilerElements();
}
