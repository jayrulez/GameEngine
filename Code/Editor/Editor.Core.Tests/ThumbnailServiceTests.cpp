// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ThumbnailService tests. A stub generator, a real EditorJobService (Update pumped as in
// JobServiceTests) and a scratch cache dir exercise the full pipeline headlessly: the icon
// fallback until ready (Get is empty, then a drawable and a single ready signal), negative
// caching (an unknown id, an unsupported type or a failed generate never reschedules), the
// content-hash disk round-trip (a second service instance serves from disk without invoking the
// generator), and RAM-only mode while the content hash is unknown.
//
// The stub resolve returns one real Instance for the id the test declares known. The service
// only reads it through TypeName() and Name(), so the fixture builds it over a ContentDatabase
// on an empty scratch mount.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.gui;
import editor.core;
import foundation.vfs;
import foundation.scene;    // the stub scene generator's Stage signature
import foundation.resource; // (never driven here; the stage owns the GPU side)

using namespace foundation::core;
using namespace editor;
namespace content = foundation::content;
namespace image = foundation::image;

namespace
{
    // Pump the light lane to completion (bounded so a hang fails, not spins).
    void PumpLight(EditorJobService& jobs)
    {
        usize guard = 0;
        while (jobs.IsLightBusy() && guard++ < 2'000'000)
        {
            jobs.Update();
        }
        jobs.Update(); // drain the completed queue
    }

    class StubGenerator final : public IThumbnailGenerator
    {
    public:
        int* prepareCount = nullptr;
        int* generateCount = nullptr;
        bool failGenerate = false;

        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"StubAsset"};
            return Span<const StringView>(kTypes, 1);
        }
        usize* payloadSeen = nullptr; // Generate: the payload size the worker assembled
        bool handStream = false;      // Prepare: also hand the worker a 100-byte stream to read
        [[nodiscard]] Status Prepare(content::Instance&, foundation::vfs::IFileSystem&,
                                     ThumbnailPrepared& out) override
        {
            if (prepareCount != nullptr)
            {
                ++*prepareCount;
            }
            out.header.PushBack(byte{42});
            if (handStream)
            {
                UniquePtr<MemoryStream> stream = MakeUnique<MemoryStream>(DefaultAllocator());
                Array<byte> bytes;
                bytes.Resize(100, byte{7});
                (void)stream->Write(bytes.Data(), bytes.Size());
                (void)stream->Seek(0, SeekOrigin::Begin);
                out.stream = UniquePtr<IStream>(Move(stream)); // move from derived to base
            }
            return Status{};
        }
        [[nodiscard]] Status Generate(Span<const byte> payload, image::Image& out) override
        {
            if (generateCount != nullptr)
            {
                ++*generateCount;
            }
            if (payloadSeen != nullptr)
            {
                *payloadSeen = payload.Size();
            }
            if (failGenerate)
            {
                return Status{ErrorCode::Internal};
            }
            out = image::Image(8, 8, image::PixelFormat::RGBA8);
            Span<u8> px = out.PixelDataMut();
            for (usize i = 0; i < px.Size(); i += 4)
            {
                px[i + 0] = 200;
                px[i + 3] = 255;
            }
            return Status{};
        }
    };

    struct Fixture
    {
        content::ContentDatabase db;
        content::Instance instance;
        EditorJobService jobs{DefaultAllocator()};
        ThumbnailService service;
        Guid known{0x1111, 0x2222};
        int prepares = 0;
        int generates = 0;
        usize payloadSeen = 0; // what Generate received (the header and the worker's stream read)
        explicit Fixture(StringView cacheDir, u64 hash = 0x77, bool failGenerate = false,
                         bool handStream = false)
            : db(DefaultAllocator(), NullMount(), nullptr, u8"asset"),
              instance(db, *db.RootGroup(), Guid{0x1111, 0x2222}, u8"Stub", u8"tests",
                       u8"StubAsset")
        {
            (void)CreateDirectory(cacheDir);
            auto generator = MakeUnique<StubGenerator>(DefaultAllocator());
            generator->prepareCount = &prepares;
            generator->generateCount = &generates;
            generator->failGenerate = failGenerate;
            generator->payloadSeen = &payloadSeen;
            generator->handStream = handStream;
            service.RegisterGenerator(Move(generator));
            content::Instance* inst = &instance;
            Guid knownId = known;
            service.Configure(
                cacheDir,
                Function<content::Instance*(const Guid&)>{
                    [inst, knownId](const Guid& id)
                    { return id == knownId ? inst : nullptr; }},
                &jobs, Function<u64(const Guid&)>{[hash](const Guid&) { return hash; }},
                u8"thumbs_empty_mount");
        }

        // An empty dedicated mount: ContentDatabase's constructor scans the mount, and the null
        // serializer factory is only safe if the scan finds no envelopes.
        static foundation::vfs::NativeFileSystem& NullMount()
        {
            (void)CreateDirectory(u8"thumbs_empty_mount");
            static foundation::vfs::NativeFileSystem fs{StringView(u8"thumbs_empty_mount"),
                                                        DefaultAllocator()};
            return fs;
        }
    };
}

TEST_CASE("thumbnails: icon-first, then the drawable + ONE ready signal")
{
    // Scratch persists across runs, and a stale cache file would serve and skip Generate.
    (void)RemoveDirectoryRecursive(u8"thumbs_happy");
    Fixture f(u8"thumbs_happy");
    int ready = 0;
    f.service.OnThumbnailReady = [&ready](const Guid&) { ++ready; };

    CHECK(!f.service.Get(f.known)); // miss schedules; caller keeps its icon
    PumpLight(f.jobs);
    RefPtr<foundation::gui::Drawable> thumb = f.service.Get(f.known);
    CHECK(thumb);
    CHECK(ready == 1);
    CHECK(f.generates == 1);
    CHECK(f.service.Get(f.known).Get() == thumb.Get()); // stable instance, no rescheduling
}

TEST_CASE("thumbnails: the worker reads the stream Prepare opened; Generate sees header + bytes")
{
    // Prepare runs on the main thread and is cheap: it opens the source and composes a header,
    // and the light worker does the read (reading whole 4k textures on the main thread freezes
    // the grid view). The stub hands a 100-byte stream behind a 1-byte header.
    (void)RemoveDirectoryRecursive(u8"thumbs_stream");
    Fixture f(u8"thumbs_stream", 0, /*failGenerate*/ false, /*handStream*/ true);
    CHECK(!f.service.Get(f.known));
    PumpLight(f.jobs);
    CHECK(f.service.Get(f.known));
    CHECK(f.prepares == 1);
    CHECK(f.generates == 1);
    CHECK(f.payloadSeen == 101u); // the header byte, then the stream's 100
}

TEST_CASE("thumbnails: negatives never re-schedule")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_negative");
    (void)RemoveDirectoryRecursive(u8"thumbs_fail");
    Fixture f(u8"thumbs_negative");
    // Unknown id: negative entry, no job.
    CHECK(!f.service.Get(Guid{0x9, 0x9}));
    CHECK(!f.service.Get(Guid{0x9, 0x9}));
    CHECK(f.service.CachedCount() == 1);
    CHECK(f.prepares == 0);

    // Failed generate: negative after the pump; no retry without Invalidate.
    Fixture g(u8"thumbs_fail", 0x77, /*failGenerate=*/true);
    CHECK(!g.service.Get(g.known));
    PumpLight(g.jobs);
    CHECK(!g.service.Get(g.known));
    CHECK(g.generates == 1);
    PumpLight(g.jobs);
    CHECK(g.generates == 1); // negative held

    // Invalidate re-arms.
    g.service.Invalidate(g.known);
    CHECK(!g.service.Get(g.known));
    PumpLight(g.jobs);
    CHECK(g.generates == 2);
}

TEST_CASE("thumbnails: content-hash disk cache round-trips without regenerating")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_disk");
    const Guid id{0x1111, 0x2222};
    {
        Fixture f(u8"thumbs_disk", 0xabc);
        (void)f.service.Get(id);
        PumpLight(f.jobs);
        CHECK(f.service.Get(id));
        CHECK(f.generates == 1);
    }
    {
        // A fresh service (new session): the <guid>-<hash>.png file serves and Generate is not
        // called (Prepare is skipped too, because the disk file short-circuits it).
        Fixture f(u8"thumbs_disk", 0xabc);
        (void)f.service.Get(id);
        PumpLight(f.jobs);
        CHECK(f.service.Get(id));
        CHECK(f.generates == 0);
        CHECK(f.prepares == 0);
    }
    {
        // A different hash (content changed): the old file mismatches, so it regenerates.
        Fixture f(u8"thumbs_disk", 0xdef);
        (void)f.service.Get(id);
        PumpLight(f.jobs);
        CHECK(f.service.Get(id));
        CHECK(f.generates == 1);
    }
}

TEST_CASE("thumbnails: unknown content hash = RAM-only (no cache file to mismatch later)")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_ram");
    const Guid id{0x1111, 0x2222};
    {
        Fixture f(u8"thumbs_ram", 0);
        (void)f.service.Get(id);
        PumpLight(f.jobs);
        CHECK(f.service.Get(id));
        CHECK(f.generates == 1);
    }
    {
        // Nothing persisted: a fresh session regenerates.
        Fixture f(u8"thumbs_ram", 0);
        (void)f.service.Get(id);
        PumpLight(f.jobs);
        CHECK(f.generates == 1);
    }
}

TEST_CASE("thumbnails: a corrupt cache file self-heals (deleted + regenerated, never a negative)")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_corrupt");
    const Guid id{0x1111, 0x2222};
    String path;
    {
        Fixture f(u8"thumbs_corrupt", 0x55);
        (void)f.service.Get(id);
        PumpLight(f.jobs);
        CHECK(f.service.Get(id));
        path = Format(u8"{}/{}-{}.png", StringView(u8"thumbs_corrupt"), id, u64{0x55});
        CHECK(FileExists(path.AsView()));
    }
    // Corrupt the cache file: the load fails, and because the file existed Prepare was
    // skipped. The service must not cache a permanent negative here.
    (void)WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>("junk"), 4));
    {
        Fixture f(u8"thumbs_corrupt", 0x55);
        CHECK(!f.service.Get(id)); // schedules; file exists so Prepare skipped
        PumpLight(f.jobs);
        // Self-heal pass: the stale file was deleted and nothing was cached...
        CHECK(!FileExists(path.AsView()));
        CHECK(f.service.CachedCount() == 0);
        // ...so the next Get takes the full Prepare and Generate path and succeeds.
        CHECK(!f.service.Get(id));
        PumpLight(f.jobs);
        CHECK(f.service.Get(id));
        CHECK(f.generates == 1);
        CHECK(FileExists(path.AsView())); // rewritten
    }
}

// ---- the GPU lane (scene generators and the one-in-flight job queue) -----------------------

namespace
{
    class StubSceneGenerator final : public ISceneThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"StubGpuAsset"};
            return Span<const StringView>(kTypes, 1);
        }
        [[nodiscard]] ThumbnailStageStep Stage(const Guid&, foundation::scene::Scene&,
                                               foundation::resource::ResourceManager&,
                                               ThumbnailFraming&) override
        {
            return ThumbnailStageStep::Failed; // never driven here: the stage owns Stage()
        }
        void Unstage(foundation::scene::Scene&) override {}
    };

    class OtherSceneGenerator final : public ISceneThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"OtherGpuAsset"};
            return Span<const StringView>(kTypes, 1);
        }
        [[nodiscard]] bool NeedsPrivateScene() const override { return true; }
        [[nodiscard]] ThumbnailStageStep Stage(const Guid&, foundation::scene::Scene&,
                                               foundation::resource::ResourceManager&,
                                               ThumbnailFraming&) override
        {
            return ThumbnailStageStep::Failed;
        }
        void Unstage(foundation::scene::Scene&) override {}
    };

    [[nodiscard]] image::Image SolidTile(u8 red)
    {
        image::Image out(8, 8, image::PixelFormat::RGBA8);
        Span<u8> px = out.PixelDataMut();
        for (usize i = 0; i < px.Size(); i += 4)
        {
            px[i + 0] = red;
            px[i + 3] = 255;
        }
        return out;
    }

    struct GpuFixture
    {
        content::ContentDatabase db;
        content::Instance instance;
        EditorJobService jobs{DefaultAllocator()};
        ThumbnailService service;
        Guid known{0x3333, 0x4444};
        u64 hash; // the content hash the service sees (0: not cooked yet)

        explicit GpuFixture(StringView cacheDir, u64 contentHash = 0x99)
            : db(DefaultAllocator(), Fixture::NullMount(), nullptr, u8"asset"),
              instance(db, *db.RootGroup(), Guid{0x3333, 0x4444}, u8"StubGpu", u8"tests",
                       u8"StubGpuAsset"),
              hash(contentHash)
        {
            (void)CreateDirectory(cacheDir);
            service.RegisterSceneGenerator(MakeUnique<StubSceneGenerator>(DefaultAllocator()));
            content::Instance* inst = &instance;
            Guid knownId = known;
            service.Configure(
                cacheDir,
                Function<content::Instance*(const Guid&)>{
                    [inst, knownId](const Guid& id)
                    { return id == knownId ? inst : nullptr; }},
                &jobs, Function<u64(const Guid&)>{[this](const Guid&) { return hash; }},
                u8"thumbs_empty_mount");
        }
    };
}

TEST_CASE("thumbnails: a scene-generated type queues ONE GPU job on miss (deduped, one taken)")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_queue");
    GpuFixture fx(u8"thumbs_gpu_queue");
    CHECK(fx.service.SceneGeneratorCount() == 1u);

    CHECK(fx.service.Get(fx.known).Get() == nullptr); // miss: queued for the stage
    CHECK(fx.service.QueuedSceneJobs() == 1u);
    CHECK(fx.service.Get(fx.known).Get() == nullptr); // re-query does not duplicate
    CHECK(fx.service.QueuedSceneJobs() == 1u);

    SceneThumbnailJob job = fx.service.TakeSceneJob();
    CHECK(job.id == fx.known);
    CHECK(job.generator != nullptr);
    CHECK(fx.service.TakeSceneJob().id.IsNil()); // one job in flight at a time
    CHECK(fx.service.Get(fx.known).Get() == nullptr); // active job: no re-queue
    CHECK(fx.service.QueuedSceneJobs() == 1u);        // (the taken job still counts)

    int ready = 0;
    fx.service.OnThumbnailReady = [&ready](const Guid&) { ++ready; };
    fx.service.AcceptSceneResult(fx.known, SolidTile(180), true);
    CHECK(ready == 1);
    CHECK(fx.service.Get(fx.known).Get() != nullptr); // published immediately (RAM)
    CHECK(fx.service.QueuedSceneJobs() == 0u);

    PumpLight(fx.jobs); // the PNG persist runs on the light lane
    bool fileExists = false;
    {
        foundation::vfs::NativeFileSystem cache{StringView(u8"thumbs_gpu_queue"),
                                                DefaultAllocator()};
        Array<foundation::vfs::DirEntry> entries;
        (void)cache.AsEnumerable()->Enumerate(u8"", entries);
        for (const auto& entry : entries)
        {
            fileExists = fileExists || entry.name.AsView().EndsWith(u8".png");
        }
    }
    CHECK(fileExists);
}

// A new project's first open: its meshes are not cooked yet (no content hash), so a scene
// thumbnail would bind a missing product and fail, warning twice per asset. It waits quietly
// instead, and the cook finishing (InvalidateAll) lets the next Get queue it.
TEST_CASE("thumbnails: a scene-generated type waits for its cook rather than failing")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_uncooked");
    GpuFixture fx(u8"thumbs_gpu_uncooked", /*contentHash*/ 0);
    CHECK(fx.service.Get(fx.known).Get() == nullptr);
    CHECK(fx.service.QueuedSceneJobs() == 0u); // nothing to stage yet
    CHECK(fx.service.Get(fx.known).Get() == nullptr);
    CHECK(fx.service.QueuedSceneJobs() == 0u); // and no retry every frame
    fx.hash = 0x77;                            // the cook finished
    fx.service.InvalidateAll();
    CHECK(fx.service.Get(fx.known).Get() == nullptr);
    CHECK(fx.service.QueuedSceneJobs() == 1u); // now it renders
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_uncooked");
}

TEST_CASE("thumbnails: a failed stage result caches a negative and never re-queues")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_fail");
    GpuFixture fx(u8"thumbs_gpu_fail");
    (void)fx.service.Get(fx.known);
    SceneThumbnailJob job = fx.service.TakeSceneJob();
    REQUIRE(job.id == fx.known);
    fx.service.AcceptSceneResult(fx.known, image::Image{}, false);
    CHECK(fx.service.Get(fx.known).Get() == nullptr);
    CHECK(fx.service.QueuedSceneJobs() == 0u); // negative: the miss stopped rescheduling
}

TEST_CASE("thumbnails: the scene-generated disk cache round-trips through the light lane")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_disk");
    const StringView dir = u8"thumbs_gpu_disk";
    {
        GpuFixture fx(dir);
        (void)fx.service.Get(fx.known);
        (void)fx.service.TakeSceneJob();
        fx.service.AcceptSceneResult(fx.known, SolidTile(90), true);
        PumpLight(fx.jobs); // persist
    }
    // A fresh service (same cache dir and hash): the file serves on the light lane, and the GPU
    // queue is never touched.
    GpuFixture fx(dir);
    CHECK(fx.service.Get(fx.known).Get() == nullptr); // scheduled, not yet loaded
    CHECK(fx.service.QueuedSceneJobs() == 0u);
    PumpLight(fx.jobs);
    CHECK(fx.service.Get(fx.known).Get() != nullptr);
    CHECK(fx.service.QueuedSceneJobs() == 0u);
}

TEST_CASE("thumbnails: Reset clears the GPU queue and drops a taken job's result")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_reset");
    GpuFixture fx(u8"thumbs_gpu_reset");
    (void)fx.service.Get(fx.known);
    SceneThumbnailJob job = fx.service.TakeSceneJob();
    REQUIRE(job.id == fx.known);
    fx.service.Reset();
    CHECK(fx.service.QueuedSceneJobs() == 0u);
    fx.service.AcceptSceneResult(job.id, SolidTile(50), true); // dropped, not published
    CHECK(fx.service.CachedCount() == 0u);
}

TEST_CASE("thumbnails: jobs route to the generator covering the asset's type")
{
    (void)RemoveDirectoryRecursive(u8"thumbs_gpu_route");
    GpuFixture fx(u8"thumbs_gpu_route");
    fx.service.RegisterSceneGenerator(MakeUnique<OtherSceneGenerator>(DefaultAllocator()));
    CHECK(fx.service.SceneGeneratorCount() == 2u);

    CHECK(fx.service.Get(fx.known).Get() == nullptr); // StubGpuAsset: queued
    const SceneThumbnailJob job = fx.service.TakeSceneJob();
    REQUIRE(job.generator != nullptr);
    // The job carries the stub generator (which covers "StubGpuAsset"), not the other one, and
    // the stub keeps the shared-stage default while the other opts into a private scene.
    CHECK(job.generator->AssetTypeNames()[0] == StringView(u8"StubGpuAsset"));
    CHECK(!job.generator->NeedsPrivateScene());

    ThumbnailFraming framing;
    CHECK(framing.radius == 1.0f);
    CHECK(!framing.preferSceneCamera);
    CHECK(framing.prewarmSteps == 0u);
    fx.service.Reset();
}
