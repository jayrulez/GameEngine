// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview - :thumbnail_stage partition.
//
// The GPU half of asset thumbnails: a persistent hidden preview scene and an offscreen render and
// readback pipeline that turn ThumbnailService's queued scene jobs into 128x128 pixels. The
// service owns generators, queueing and publication; this stage owns all the GPU work: the scene
// the generators populate, an orthographic camera framed from the generator's bounds (eye along
// (1,1,1), half-extent radius * 1.1, so the framing does not depend on rotation), a 4x
// supersampled target (a 512 to 128 box downscale gives anti-aliasing for free), and a
// copy-to-buffer readback retired by the frame ring. The copy is recorded on the frame encoder,
// so it is ordered after the render; when the same frame index comes around again the
// submission has retired and the buffer maps without a stall.
//
// One job is in flight at a time (ThumbnailService::TakeSceneJob enforces it); a generator whose
// resources are still resolving returns Pending and is retried each frame within a bounded
// budget. The interface stays lean and PIMPL'd for the same reason as PreviewViewport's: heavy
// page translation units import this module, so engine.render and engine.scene must stay out.

module;
#include "Core/Prelude.h"

export module editor.preview:thumbnail_stage;

import foundation.core;
import foundation.runtime.client; // IApplicationHost
import foundation.graphics;      // FrameContext
import foundation.resource;      // ResourceManager (handed to generators)
import editor.core;              // ThumbnailService (jobs in, pixels out)

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;

    /// The offscreen GPU thumbnail renderer. One per open project, app-owned beside the
    /// ThumbnailService; construct after the project's runtime context exists, Shutdown
    /// (or destroy) before the render subsystem goes away.
    class ThumbnailStage
    {
    public:
        /// Supersample factor: render at kSupersample * kThumbnailServiceThumbnailSize and
        /// box-downscale, so edges land antialiased without MSAA machinery.
        static constexpr u32 kSupersample = 4;
        /// Frames a Pending generator may retry before the job fails (a missing product never
        /// resolves; the negative cache stops the rescheduling loop).
        static constexpr u32 kMaxStagingFrames = 600;

        ThumbnailStage(runtime::IApplicationHost& host, ThumbnailService& service,
                       foundation::resource::ResourceManager* resources);
        ~ThumbnailStage();

        ThumbnailStage(const ThumbnailStage&) = delete;
        ThumbnailStage& operator=(const ThumbnailStage&) = delete;

        /// Main thread, once per frame before rendering: takes and stages the next job.
        void Update();

        /// Called inside the app's scene-render bracket (between BeginRendering and EndRendering,
        /// on main window frames): renders a staged job, encodes its readback, and retires a
        /// pending readback whose ring slot has come back around.
        void Render(foundation::graphics::FrameContext& frame);

        /// Drops the GPU objects and the preview scene. Also called by the destructor.
        void Shutdown();

        /// True while a job is staged, rendering, or awaiting readback (for tests).
        [[nodiscard]] bool IsBusy() const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}
