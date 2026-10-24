// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Core - :thumbnail_service partition
//
// Asset thumbnails: an editor-only service that resolves a content
// instance's Guid to a small preview drawable. Icon-first display is the CONSUMER's job - Get()
// returns empty until a thumbnail exists and OnThumbnailReady fires the swap-in; nothing here
// ever blocks the UI.
//
// Mechanics: per-asset-type generators (registered by the app; unknown types simply keep their
// icon) run on the EditorJobService LIGHT lane; results land in a content-keyed disk cache
// (<project>/.cache/thumbs/<guid>-<recipeHash>.png - a stale hash is detected by filename
// mismatch and the old file replaced; deleting the directory is always safe) and a RAM map of
// OWNING drawables. OwnedThumbnailDrawable owns its pixels, so map eviction can never dangle a
// drawable already handed to the UI (the bind-group value-keyed-cache lesson: renderer caches
// key on ImageData::InstanceId, and the pixels live exactly as long as the drawable).
//
// Threading: Get/Invalidate/OnThumbnailReady are MAIN-thread; generation + PNG IO run on the
// light worker; completion fires on the main thread from EditorJobService::Update. In-flight
// jobs use the heap-slot lifetime pattern so project close or service reset
// mid-flight is safe. A generator's Prepare runs on the MAIN thread but must stay CHEAP: it
// reads the small asset envelope and OPENS the source it needs (a file under Sources/, a
// sidecar stream) - the worker does the reading. Until 2026-09-23 Prepare read the whole
// source file (a 4k JPEG, a WAV) on the main thread, up to kMaxInFlight of them per Get
// burst, and the asset browser's grid view froze for as long as thumbnails were pending.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.core:thumbnail_service;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.resource; // ResourceManager (scene-thumbnail generators bind cooked products)
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
    /// A thumbnail drawable that OWNS its pixels: the image lives exactly as long as the
    /// drawable, so the service's RAM map can drop entries freely while the UI still holds refs.
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

    /// What Prepare hands the worker: a small header the generator composes on the main
    /// thread (tags, dimensions) and a stream OPENED there but READ on the worker. The
    /// worker's payload = the header bytes followed by the whole stream (either may be absent).
    struct ThumbnailPrepared
    {
        Array<byte> header;
        UniquePtr<IStream> stream;
        [[nodiscard]] bool HasInput() const noexcept
        {
            return !header.IsEmpty() || stream.Get() != nullptr;
        }
    };

    /// One per-asset-type thumbnail producer. Split across threads:
    /// Prepare runs on the MAIN thread and resolves what the worker needs (content
    /// Instance/DB access is main-thread-only - a cook or delete can run concurrently with the
    /// light lane) WITHOUT reading it: it opens the source into a stream the worker reads.
    /// Generate runs on the LIGHT worker over the read payload: CPU only, no UI, no GPU
    /// (offscreen renders are the preview-bake path). The output image should already be
    /// thumbnail-sized (the service saves it verbatim).
    class IThumbnailGenerator
    {
    public:
        virtual ~IThumbnailGenerator() = default;
        /// The content asset-type names this generator covers (e.g. "TextureAsset").
        [[nodiscard]] virtual Span<const StringView> AssetTypeNames() const = 0;
        /// MAIN thread, cheap: read the small envelope and OPEN the source into `out` - a
        /// file under `sources` (the project's Sources/ mount, mount-relative Asset::fileName
        /// paths) or an instance data stream - plus any header bytes. No whole-file reads
        /// here: the worker reads `out.stream`.
        [[nodiscard]] virtual Status Prepare(content::Instance& instance,
                                             foundation::vfs::IFileSystem& sources,
                                             ThumbnailPrepared& out) = 0;
        /// LIGHT worker: produce the thumbnail pixels from the payload (header + stream bytes).
        [[nodiscard]] virtual Status Generate(Span<const byte> payload, image::Image& out) = 0;
    };

    /// One step of staging an asset into the shared preview scene.
    enum class ThumbnailStageStep : u8
    {
        Pending, // resources still resolving - call Stage again next frame
        Ready,   // scene populated; outFraming carries the view parameters
        Failed,  // cannot stage (missing product, unbound resource) - negative-cache it
    };

    /// How the stage views a staged job. Default: an ORTHO camera along (1,1,1) aimed at
    /// `center`, sized from `radius`. `preferSceneCamera` asks the stage to use the scene's
    /// own primary camera when one exists (scene documents look like themselves); the framing
    /// fields stay the fallback. `prewarmSteps` simulation ticks (1/60s each) run before the
    /// render - for content that is empty at t=0 (particles).
    struct ThumbnailFraming
    {
        Float3 center{};
        f32 radius = 1.0f;
        bool preferSceneCamera = false;
        u32 prewarmSteps = 0;
    };

    /// A GPU thumbnail producer: populates an offscreen preview scene for one asset; the
    /// STAGE (editor.preview) owns the camera/render/readback around it. All calls are
    /// MAIN-thread. Stage() is called once per frame until it returns Ready or Failed (the
    /// stage bounds the retries); Unstage() must remove exactly what Stage() added.
    /// NeedsPrivateScene() = false stages into the SHARED persistent scene (cheap; the
    /// generator owns persistent entities toggled active per job). True gives each job a
    /// FRESH scene with the app's full manager set, destroyed at job end - for generators
    /// that instantiate arbitrary content (prefabs, scene documents, simulated effects)
    /// where teardown-by-hand or leaked scene-level state would be the bug.
    class ISceneThumbnailGenerator
    {
    public:
        virtual ~ISceneThumbnailGenerator() = default;
        /// The content asset-type names this generator covers (e.g. "MeshAsset").
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

    /// The thumbnail service. One per open project - the app Configures
    /// it on project open and Resets it on close.
    class ThumbnailService
    {
    public:
        /// Thumbnail edge size (square, aspect-fit letterboxed by the generators' helper).
        // ENGINE_EXPORT_DATA: odr-used from another library (shared-libraries.md P5/W1).
        static constexpr ENGINE_EXPORT_DATA u32 kThumbnailSize = 128;
        /// Scheduling budget (the cross-engine lesson): at most this many loads in flight; a
        /// bind sweep over a big folder trickles instead of flooding the light lane - the NEXT
        /// Get for a skipped id (every bind re-queries) schedules it once a slot frees.
        static constexpr usize kMaxInFlight = 8;

        [[nodiscard]] usize GeneratorCount() const noexcept { return m_generators.Size(); }

        /// The swap-in signal: a thumbnail for this Guid became available (MAIN thread).
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
        /// The next queued GPU job, or empty ({} id) when none. The job stays "taken" until
        /// AcceptSceneResult for its id - a second call while one is out returns empty (the
        /// one-in-flight rule every surveyed engine converged on).
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
        /// The stage finished (or failed) the active job: publish exactly like a CPU generate -
        /// disk cache (PNG on the light lane), RAM entry, OnThumbnailReady. Failure caches a
        /// negative entry so Get stops rescheduling until Invalidate.
        void AcceptSceneResult(const Guid& id, image::Image pixels, bool ok);
        [[nodiscard]] usize QueuedSceneJobs() const noexcept
        {
            return m_sceneQueue.Size() + (m_sceneJobActive ? 1u : 0u);
        }

        /// Project-open wiring: where the cache lives, how to resolve instances, the job lane,
        /// and the content-hash source. Any previous state is dropped.
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

        /// Project-close: drop the RAM cache and detach. In-flight jobs complete harmlessly
        /// against their slots (the alive flag is cleared).
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

        /// The resolved thumbnail, or EMPTY when none exists yet (the caller keeps its type
        /// icon). A miss schedules load-or-generate on the light lane; OnThumbnailReady fires
        /// when the swap-in is ready. Main thread only.
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

        /// Drop a cached thumbnail (cook/import invalidation); the next Get regenerates. The
        /// content-hash filenames make even a missed invalidation self-healing.
        void Invalidate(const Guid& id) { m_entries.Remove(id); }

        /// Drop the whole RAM cache (cook finished: recipe hashes moved). Unchanged assets
        /// reload from their unchanged disk files - cheap; changed ones regenerate.
        void InvalidateAll() { m_entries.Clear(); }

        [[nodiscard]] usize CachedCount() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            RefPtr<OwnedThumbnailDrawable> drawable;
        };

        // The heap slot both closures own (lifetime rule): the service dtor /
        // Reset clears serviceAlive; the completion always deletes the slot. All flags are
        // main-thread; the worker only touches `pixels` + `ok`.
        struct JobSlot
        {
            Guid id{};
            bool serviceAlive = true;      // main-thread flag (service dtor/Reset clears)
            bool ok = false;               // worker -> completion result
            bool negative = false;         // completion: cache "no thumbnail" (stop rescheduling)
            bool staleDiskFile = false;    // disk cache failed to load AND no payload to
                                           // regenerate from (Prepare was skipped because the
                                           // file existed): delete the file, retry fully
            image::Image pixels;           // worker output
            ThumbnailPrepared prepared;    // main-thread Prepare output: header + an OPEN stream
            Array<byte> payload;           // the worker's read: header + the stream's bytes
            String diskPath;               // empty = RAM-only (unknown content hash)
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
