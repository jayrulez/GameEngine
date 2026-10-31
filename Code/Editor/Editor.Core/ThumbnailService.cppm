// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Core - :thumbnail_service partition
//
// Asset thumbnails: an editor-only service that resolves a content instance's Guid to a small
// preview drawable. Showing an icon first is the consumer's job: Get() returns empty until a
// thumbnail exists, and OnThumbnailReady signals the swap. Nothing here blocks the UI.
//
// Per-asset-type generators (registered by the application; unknown types keep their icon) run on
// the EditorJobService light lane. Results land in a content-keyed disk cache
// (<project>/.cache/thumbs/<guid>-<recipeHash>.png; a stale hash shows as a filename mismatch and
// the old file is replaced, and deleting the directory is always safe) and in a RAM map of owning
// drawables. OwnedThumbnailDrawable owns its pixels, so evicting a map entry never leaves a
// drawable already handed to the UI dangling: renderer caches key on ImageData::InstanceId, and
// the pixels live exactly as long as the drawable.
//
// Threading: Get, Invalidate and OnThumbnailReady are main thread only; generation and PNG IO run
// on the light worker; completion fires on the main thread from EditorJobService::Update. In-flight
// jobs keep their state in a heap slot, so closing the project or resetting the service mid-flight
// is safe. A generator's Prepare runs on the main thread and must stay cheap: it reads the small
// asset envelope and opens the source it needs (a file under Sources/, a sidecar stream), and the
// worker does the reading. Reading whole source files (a 4k JPEG, a WAV) in Prepare would stall the
// main thread for up to kMaxInFlight files per burst of Get calls.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.core:thumbnail_service;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.resource; // ResourceManager (scene thumbnail generators bind cooked products)
import foundation.scene;    // Scene (the stage's persistent preview scene, populated by generators)
import foundation.gui; // ImageDrawable (a thumbnail as the GUI draws it)
import foundation.vfs;
import editor.project; // EditorJobService

using namespace foundation::core;
namespace content = foundation::content;
namespace image = foundation::image;
namespace gui = foundation::gui;

export namespace editor
{
    /// A thumbnail drawable that owns its pixels: the image lives exactly as long as the drawable,
    /// so the service's RAM map can drop entries freely while the UI still holds references.
    class OwnedThumbnailDrawable final : public gui::ImageDrawable
    {
        RTTI_OBJECT(OwnedThumbnailDrawable, gui::ImageDrawable)
    public:
        explicit OwnedThumbnailDrawable(image::Image pixels) : m_pixels(Move(pixels))
        {
            Image = &m_pixels;
        }

    private:
        image::Image m_pixels;
    };

    RTTI_DEFINE_OBJECT(OwnedThumbnailDrawable, "rtti::editor")

    /// What Prepare hands the worker: a small header the generator composes on the main thread
    /// (tags, dimensions) and a stream opened there but read on the worker. The worker's payload
    /// is the header bytes followed by the whole stream (either may be absent).
    struct ThumbnailPrepared
    {
        Array<byte> header;
        UniquePtr<IStream> stream;
        [[nodiscard]] bool HasInput() const noexcept
        {
            return !header.IsEmpty() || stream.Get() != nullptr;
        }
    };

    /// One per-asset-type thumbnail producer, split across threads. Prepare runs on the main
    /// thread and resolves what the worker needs without reading it (content Instance and database
    /// access is main thread only, since a cook or delete can run concurrently with the light
    /// lane): it opens the source into a stream the worker reads. Generate runs on the light worker
    /// over the read payload: CPU only, no UI and no GPU (offscreen renders go through
    /// ISceneThumbnailGenerator). The output image should already be thumbnail-sized, since the
    /// service saves it as it is.
    class IThumbnailGenerator
    {
    public:
        virtual ~IThumbnailGenerator() = default;
        /// The content asset type names this generator covers (such as "TextureAsset").
        [[nodiscard]] virtual Span<const StringView> AssetTypeNames() const = 0;
        /// Main thread, cheap: read the small envelope and open the source into `out` (a file
        /// under `sources`, the project's Sources/ mount with mount-relative Asset::fileName paths,
        /// or an instance data stream), plus any header bytes. Do not read whole files here; the
        /// worker reads `out.stream`.
        [[nodiscard]] virtual Status Prepare(content::Instance& instance,
                                             foundation::vfs::IFileSystem& sources,
                                             ThumbnailPrepared& out) = 0;
        /// Light worker: produce the thumbnail pixels from the payload (header and stream bytes).
        [[nodiscard]] virtual Status Generate(Span<const byte> payload, image::Image& out) = 0;
    };

    /// One step of staging an asset into the shared preview scene.
    enum class ThumbnailStageStep : u8
    {
        Pending, // resources still resolving: call Stage again next frame
        Ready,   // scene populated; outFraming carries the view parameters
        Failed,  // cannot stage (missing product, unbound resource): cache the failure
    };

    /// How the stage views a staged job. By default, an orthographic camera along (1,1,1) aimed at
    /// `center`, sized from `radius`. `preferSceneCamera` asks the stage to use the scene's own
    /// primary camera when one exists (so scene documents look like themselves), with the framing
    /// fields as the fallback. `prewarmSteps` simulation ticks (1/60 s each) run before the render,
    /// for content that is empty at t=0 (particles).
    struct ThumbnailFraming
    {
        Float3 center{};
        f32 radius = 1.0f;
        bool preferSceneCamera = false;
        u32 prewarmSteps = 0;
    };

    /// A GPU thumbnail producer: it populates an offscreen preview scene for one asset, and the
    /// stage (editor.preview) owns the camera, render and readback around it. All calls are on the
    /// main thread. Stage() is called once per frame until it returns Ready or Failed (the stage
    /// limits the retries); Unstage() must remove exactly what Stage() added. When
    /// NeedsPrivateScene() is false, the job stages into the shared persistent scene (cheap; the
    /// generator owns persistent entities it activates per job). When true, each job gets a fresh
    /// scene with the application's full manager set, destroyed when the job ends. That suits
    /// generators that instantiate arbitrary content (prefabs, scene documents, simulated
    /// effects), where tearing down by hand could leak scene-level state.
    class ISceneThumbnailGenerator
    {
    public:
        virtual ~ISceneThumbnailGenerator() = default;
        /// The content asset type names this generator covers (such as "MeshAsset").
        [[nodiscard]] virtual Span<const StringView> AssetTypeNames() const = 0;
        [[nodiscard]] virtual bool NeedsPrivateScene() const { return false; }
        [[nodiscard]] virtual ThumbnailStageStep Stage(const Guid& id,
                                                       foundation::scene::Scene& scene,
                                                       foundation::resource::ResourceManager& resources,
                                                       ThumbnailFraming& outFraming) = 0;
        virtual void Unstage(foundation::scene::Scene& scene) = 0;
    };

    /// One queued GPU thumbnail job (the stage drives it; the service owns the bookkeeping).
    struct SceneThumbnailJob
    {
        Guid id{};
        ISceneThumbnailGenerator* generator = nullptr; // borrowed; lives on the service
    };

    /// ThumbnailService::kThumbnailSize, at namespace scope so another library can read it (MSVC does
    /// not export a class's static constexpr member from a shared library).
    inline constexpr ENGINE_EXPORT_DATA u32 kThumbnailServiceThumbnailSize = 128;

    /// The thumbnail service, one per open project: the application calls Configure on project
    /// open and Reset on close.
    class ThumbnailService
    {
    public:
        /// Thumbnail edge size (square; the generators' helper letterboxes to fit the aspect).
        // ENGINE_EXPORT_DATA: odr-used from another library.
        static constexpr ENGINE_EXPORT_DATA u32 kThumbnailSize = kThumbnailServiceThumbnailSize;
        /// Scheduling budget: at most this many loads in flight, so a bind sweep over a big
        /// folder trickles instead of flooding the light lane. The next Get for a skipped id
        /// (every bind asks again) schedules it once a slot frees.
        static constexpr usize kMaxInFlight = 8;

        [[nodiscard]] usize GeneratorCount() const noexcept { return m_generators.Size(); }

        /// The swap signal: a thumbnail for this Guid became available (main thread).
        Function<void(const Guid&)> OnThumbnailReady;

        ~ThumbnailService() { Reset(); }

        void RegisterGenerator(UniquePtr<IThumbnailGenerator> generator)
        {
            m_generators.PushBack(Move(generator));
        }

        void RegisterSceneGenerator(UniquePtr<ISceneThumbnailGenerator> generator)
        {
            m_sceneGenerators.PushBack(Move(generator));
        }
        [[nodiscard]] usize SceneGeneratorCount() const noexcept
        {
            return m_sceneGenerators.Size();
        }

        // ---- the GPU lane (driven by the stage, one job at a time) --------------------
        /// The next queued GPU job, or empty ({} id) when none. The job stays taken until
        /// AcceptSceneResult for its id; a second call while one is out returns empty, so only one
        /// GPU job is in flight.
        [[nodiscard]] SceneThumbnailJob TakeSceneJob()
        {
            if (m_sceneJobActive || m_sceneQueue.IsEmpty())
            {
                return {};
            }
            SceneThumbnailJob job = m_sceneQueue[0];
            m_sceneQueue.RemoveAt(0);
            m_sceneJobActive = true;
            m_activeSceneJob = job.id;
            return job;
        }
        /// The stage finished (or failed) the active job. Publishes exactly as a CPU generate does:
        /// the disk cache (PNG on the light lane), the RAM entry, and OnThumbnailReady. Failure
        /// caches a negative entry so Get stops rescheduling until Invalidate.
        void AcceptSceneResult(const Guid& id, image::Image pixels, bool ok);
        [[nodiscard]] usize QueuedSceneJobs() const noexcept
        {
            return m_sceneQueue.Size() + (m_sceneJobActive ? 1u : 0u);
        }

        /// Set up on project open: where the cache lives, how to resolve instances, the job lane,
        /// and the content hash source. Any previous state is dropped.
        void Configure(StringView cacheDirectory, Function<content::Instance*(const Guid&)> resolve,
                       EditorJobService* jobs, Function<u64(const Guid&)> contentHash,
                       StringView sourcesRoot)
        {
            Reset();
            m_cacheDirectory = String(cacheDirectory);
            m_resolve = Move(resolve);
            m_jobs = jobs;
            m_contentHash = Move(contentHash);
            m_sources = MakeUnique<foundation::vfs::NativeFileSystem>(foundation::core::DefaultAllocator(),
                                                                      sourcesRoot,
                                                                      foundation::core::DefaultAllocator());
        }

        /// On project close: drop the RAM cache and detach. In-flight jobs complete harmlessly
        /// against their slots, whose alive flag is cleared.
        void Reset()
        {
            for (InFlight& flight : m_inFlight)
            {
                flight.slot->serviceAlive = false;
            }
            m_inFlight.Clear();
            m_sceneQueue.Clear();
            m_sceneJobActive = false;
            m_activeSceneJob = Guid{};
            m_entries.Clear();
            m_cacheDirectory.Clear();
            m_resolve = {};
            m_contentHash = {};
            m_jobs = nullptr;
            m_sources = {};
        }

        /// The resolved thumbnail, or empty when none exists yet (the caller keeps its type icon).
        /// A miss schedules a load or generate on the light lane, and OnThumbnailReady fires when
        /// the thumbnail is ready. Main thread only.
        [[nodiscard]] RefPtr<gui::Drawable> Get(const Guid& id)
        {
            if (id.IsNil() || m_jobs == nullptr || !m_resolve)
            {
                return {};
            }
            if (const Entry* entry = m_entries.Find(id))
            {
                return RefPtr<gui::Drawable>(entry->drawable.Get());
            }
            ScheduleLoad(id);
            return {};
        }

        /// Drop a cached thumbnail (invalidated by a cook or import); the next Get regenerates it.
        /// Because filenames carry the content hash, even a missed invalidation corrects itself.
        void Invalidate(const Guid& id) { m_entries.Remove(id); }

        /// Drop the whole RAM cache (a cook finished and recipe hashes changed). Unchanged assets
        /// reload cheaply from their unchanged disk files; changed ones regenerate.
        void InvalidateAll() { m_entries.Clear(); }

        [[nodiscard]] usize CachedCount() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            RefPtr<OwnedThumbnailDrawable> drawable;
        };

        // The heap slot both closures share: the service destructor or Reset clears
        // serviceAlive, and the completion always deletes the slot. All flags are main thread
        // only; the worker touches only `pixels` and `ok`.
        struct JobSlot
        {
            Guid id{};
            bool serviceAlive = true;      // main-thread flag (the destructor or Reset clears it)
            bool ok = false;               // result from the worker for the completion
            bool negative = false;         // completion caches "no thumbnail" (stops rescheduling)
            bool staleDiskFile = false;    // disk cache failed to load and there is no payload to
                                           // regenerate from (Prepare was skipped because the
                                           // file existed): delete the file, retry fully
            image::Image pixels;           // worker output
            ThumbnailPrepared prepared;    // main-thread Prepare output: header and an open stream
            Array<byte> payload;           // the worker's read: header and the stream's bytes
            String diskPath;               // empty means RAM only (unknown content hash)
            IThumbnailGenerator* generator = nullptr; // borrowed; generators live on the service
        };
        struct InFlight
        {
            Guid id{};
            JobSlot* slot = nullptr;
        };

        [[nodiscard]] ISceneThumbnailGenerator* SceneGeneratorFor(StringView typeName)
        {
            for (const UniquePtr<ISceneThumbnailGenerator>& generator : m_sceneGenerators)
            {
                for (StringView covered : generator->AssetTypeNames())
                {
                    if (covered == typeName)
                    {
                        return generator.Get();
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] IThumbnailGenerator* GeneratorFor(StringView typeName)
        {
            for (const UniquePtr<IThumbnailGenerator>& generator : m_generators)
            {
                for (StringView covered : generator->AssetTypeNames())
                {
                    if (covered == typeName)
                    {
                        return generator.Get();
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] String CachePathFor(const Guid& id, u64 hash) const
        {
            return Format(u8"{}/{}-{}.png", m_cacheDirectory, id, hash);
        }

        void ScheduleLoad(const Guid& id);
        void CompleteLoad(JobSlot* slot);

        String m_cacheDirectory;
        UniquePtr<foundation::vfs::NativeFileSystem> m_sources; // the project Sources/ mount
        Function<content::Instance*(const Guid&)> m_resolve;
        Function<u64(const Guid&)> m_contentHash;
        EditorJobService* m_jobs = nullptr;
        Array<UniquePtr<IThumbnailGenerator>> m_generators;
        Array<UniquePtr<ISceneThumbnailGenerator>> m_sceneGenerators;
        HashMap<Guid, Entry> m_entries;
        Array<InFlight> m_inFlight;
        Array<SceneThumbnailJob> m_sceneQueue; // GPU-lane queue (one active at a time)
        bool m_sceneJobActive = false;
        Guid m_activeSceneJob{};
    };
}
