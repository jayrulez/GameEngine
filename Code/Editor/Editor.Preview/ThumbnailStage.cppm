// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview - :thumbnail_stage partition.
//
// The GPU half of asset thumbnails: a persistent hidden preview scene plus an offscreen
// render-and-readback pipeline that turns ThumbnailService's queued scene jobs into 128x128
// pixels. The service owns generators, queueing and publication; this stage owns everything
// GPU: the scene the generators populate, an ortho camera framed from the generator's bounds
// (eye along (1,1,1), half-extent = radius * 1.1 - rotation-invariant framing), a 4x
// supersampled target (512 -> 128 box downscale for free anti-aliasing), and a copy-to-buffer
// readback retired by frame-ring round-trip (the copy rides the frame encoder, so it is
// ordered after the render; when the same frame index comes around again the submission has
// provably retired and the buffer maps without a stall).
//
// One job is in flight at a time (ThumbnailService::TakeSceneJob enforces it); a generator
// whose resources are still resolving returns Pending and is retried each frame under a
// bounded budget. The interface stays lean and PIMPL'd for the same reason PreviewViewport's
// does: heavy page TUs import this module, so engine.render/engine.scene must not leak in.

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

        /// Main thread, once per frame BEFORE rendering: take/stage the next job.
        void Update();

        /// Inside the app's scene-render bracket (between BeginRendering and EndRendering,
        /// main window frames): render a staged job, encode its readback, retire a pending
        /// readback whose ring slot came back around.
        void Render(foundation::graphics::FrameContext& frame);

        /// Drop GPU objects + the preview scene. Also called by the destructor.
        void Shutdown();

        /// True while a job is staged, rendering, or awaiting readback (test seam).
        [[nodiscard]] bool IsBusy() const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}
