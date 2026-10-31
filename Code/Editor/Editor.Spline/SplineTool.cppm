// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Editor::Spline - `editor.spline`: the interactive spline viewport tool.
///
/// Control-point editing for SplineComponent on the IViewportTool framework. Points drag on a
/// camera-facing plane, Ctrl+click on a segment inserts at the closest curve position, and Delete
/// or X removes the hovered point (never going below two points). Every gesture is one undo
/// command holding before and after snapshots of the point set, keyed by entity guid. Points are
/// in entity-local space; the entity transform places the whole curve. The auto handles and the
/// arc-length table are rebuilt after every change, so consumers always see a coherent curve.

module;
#include "Core/Prelude.h"

export module editor.spline;

import foundation.core;
import editor.viewporttools;

using namespace foundation::core;

export namespace editor
{
    /// The provider that adds the spline tool to every scene viewport. Tools.Editor registers it
    /// explicitly; it is not discovered.
    class SplineViewportToolProvider final : public IViewportToolProvider
    {
    public:
        void CreateTools(ViewportToolManager& manager,
                         const ViewportToolHostContext& context) override;
    };

    /// Register the spline viewport tool (call once at editor start, from Tools.Editor).
    void RegisterSplineViewportTools();

    /// Creates the spline tool for one host (what the provider adds to a viewport's manager), for
    /// a host that drives it directly, such as a headless test.
    [[nodiscard]] UniquePtr<IViewportTool> CreateSplineEditTool(const ViewportToolHostContext& context);

    /// What the spline tool is doing, read from an instance whose Id identifies it: the hovered
    /// and selected point (-1 for none), whether a drag is in progress, and whether Ctrl shows an
    /// insert preview. A tool that is not the spline tool reads as all defaults.
    struct SplineEditToolState
    {
        i32 hoverPoint = -1;
        i32 selectedPoint = -1;
        bool dragging = false;
        bool insertPreview = false;
        bool placePreview = false; // Ctrl over a curve with no segment, where a click appends a point
    };
    [[nodiscard]] SplineEditToolState SplineEditToolStateOf(const IViewportTool& tool);
}
