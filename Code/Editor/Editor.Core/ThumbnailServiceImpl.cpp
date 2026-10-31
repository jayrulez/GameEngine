// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Core - ThumbnailService implementation.
//
// ScheduleLoad gathers everything on the main thread (instance resolve, generator lookup, the
// Prepare payload, the content-hash cache path); the light worker loads from disk or generates and
// writes the PNG; and CompleteLoad publishes the drawable and fires OnThumbnailReady back on the
// main thread. Failures cache a negative entry so per-frame Get calls never reschedule a known
// miss; Invalidate clears it. Teardown order: the application shuts down the job service (joining
// the light worker) before destroying this service, because the worker borrows the generator.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.core;

import foundation.core;
import foundation.content;
import foundation.image.io;

namespace editor
{
    using namespace foundation::core;
    namespace content = foundation::content; // the partition's alias is not visible here on MSVC
    namespace image = foundation::image;

    void ThumbnailService::ScheduleLoad(const Guid& id)
    {
        if (m_inFlight.Size() >= kMaxInFlight)
        {
            return; // budget: the next Get retries once the lane drains
        }
        for (const InFlight& flight : m_inFlight)
        {
            if (flight.id == id)
            {
                return; // already on its way
            }
        }
        content::Instance* instance = m_resolve(id);
        if (instance == nullptr)
        {
            m_entries.InsertOrAssign(id, Entry{}); // unknown id: negative entry (Invalidate clears it)
            return;
        }
        IThumbnailGenerator* generator = GeneratorFor(instance->TypeName());
        ISceneThumbnailGenerator* sceneGenerator =
            (generator == nullptr) ? SceneGeneratorFor(instance->TypeName()) : nullptr;
        if (generator == nullptr && sceneGenerator == nullptr)
        {
            m_entries.InsertOrAssign(id, Entry{}); // no generator: the type icon stays
            return;
        }

        auto* slot = editor::EditorRootAllocator().New<JobSlot>();
        slot->id = id;
        slot->generator = generator;
        // An unknown content hash (mid-cook, no record yet) means RAM only: never write a cache
        // file that can never match again. The invalidation when the cook finishes regenerates
        // with the real hash and then persists.
        const u64 hash = m_contentHash ? m_contentHash(id) : 0;
        if (hash != 0)
        {
            slot->diskPath = CachePathFor(id, hash);
        }

        const bool diskHit = !slot->diskPath.IsEmpty() && FileExists(slot->diskPath.AsView());

        // A GPU-generated type with no cached file goes to the scene queue (one job at a
        // time, drained by the stage); a cached file loads on the light lane like any other.
        if (generator == nullptr)
        {
            if (!diskHit)
            {
                editor::EditorRootAllocator().Delete(slot);
                if (hash == 0)
                {
                    // Not cooked yet (a new project's first cook is still running): the stage would
                    // bind a product that is not there and fail. Wait quietly; the InvalidateAll
                    // when the cook finishes drops this entry, and the next Get queues the job.
                    m_entries.InsertOrAssign(id, Entry{});
                    return;
                }
                if (m_activeSceneJob == id)
                {
                    return; // being rendered right now
                }
                for (const SceneThumbnailJob& queued : m_sceneQueue)
                {
                    if (queued.id == id)
                    {
                        return; // already queued
                    }
                }
                m_sceneQueue.PushBack(SceneThumbnailJob{id, sceneGenerator});
                return;
            }
            // Load-only slot: slot->generator stays null and the worker never generates. A stale
            // file corrects itself through the staleDiskFile path (delete and retry in full, which
            // then queues the GPU job).
        }
        // Prepare runs on the main thread, so the worker never touches the content database. A
        // Prepare failure (a bad or missing source stream) is a negative entry, logged once here
        // rather than every frame.
        else if (!diskHit)
        {
            const Status prepared = generator->Prepare(*instance, *m_sources, slot->prepared);
            if (!prepared.IsOk())
            {
                LOG_WARNING(u8"Thumbnails", u8"prepare failed for '{}' ({}): error {}",
                            instance->Name(), instance->TypeName(),
                            static_cast<u32>(prepared.Code()));
                m_entries.InsertOrAssign(id, Entry{});
                editor::EditorRootAllocator().Delete(slot);
                return;
            }
        }

        m_inFlight.PushBack(InFlight{id, slot});
        ThumbnailService* self = this;
        m_jobs->SubmitLight(
            Function<void()>{[slot]()
                             {
                                 // Light worker: disk cache first, else generate and persist.
                                 if (!slot->diskPath.IsEmpty() &&
                                     image::io::LoadImage(slot->diskPath.AsView(), slot->pixels)
                                         .IsOk())
                                 {
                                     slot->ok = true;
                                     return;
                                 }
                                 if (slot->generator == nullptr ||
                                     (!slot->prepared.HasInput() && !slot->diskPath.IsEmpty()))
                                 {
                                     // Either a load-only slot (GPU-generated type) whose file
                                     // would not load, or the cache file existed at schedule
                                     // time (so Prepare was skipped) but is stale or corrupt.
                                     // Delete it and retry the full path on the next Get.
                                     slot->staleDiskFile = true;
                                     return;
                                 }
                                 // The read happens here, on the worker: the header the
                                 // generator composed, then the stream it opened.
                                 slot->payload = Move(slot->prepared.header);
                                 if (slot->prepared.stream.Get() != nullptr)
                                 {
                                     IStream& stream = *slot->prepared.stream;
                                     const i64 size = stream.Size();
                                     if (size <= 0)
                                     {
                                         slot->negative = true;
                                         return;
                                     }
                                     const usize start = slot->payload.Size();
                                     slot->payload.Resize(start + static_cast<usize>(size));
                                     const u64 read = stream.Read(slot->payload.Data() + start,
                                                                  static_cast<u64>(size));
                                     slot->prepared.stream.Reset(); // closed on the worker
                                     if (read != static_cast<u64>(size))
                                     {
                                         slot->negative = true;
                                         return;
                                     }
                                 }
                                 const Status generated =
                                     slot->generator->Generate(Span<const byte>(slot->payload.Data(),
                                                                                slot->payload.Size()),
                                                               slot->pixels);
                                 if (!generated.IsOk())
                                 {
                                     slot->negative = true;
                                     return;
                                 }
                                 slot->ok = true;
                                 if (!slot->diskPath.IsEmpty())
                                 {
                                     (void)image::io::SaveImage(slot->pixels,
                                                                slot->diskPath.AsView(),
                                                                image::io::ImageFileFormat::PNG);
                                 }
                             }},
            Function<void()>{[self, slot]() { self->CompleteLoad(slot); }});
    }

    void ThumbnailService::CompleteLoad(JobSlot* slot)
    {
        // Main thread. The service may have been Reset (project closed) mid-flight; the slot's
        // flag is the guard and the slot is always deleted here.
        if (slot->serviceAlive)
        {
            for (usize i = 0; i < m_inFlight.Size(); ++i)
            {
                if (m_inFlight[i].slot == slot)
                {
                    m_inFlight.RemoveAt(i);
                    break;
                }
            }
            if (slot->ok)
            {
                Entry entry;
                entry.drawable = MakeRef<OwnedThumbnailDrawable>(editor::EditorRootAllocator(),
                                                                 Move(slot->pixels));
                m_entries.InsertOrAssign(slot->id, Move(entry));
                if (OnThumbnailReady)
                {
                    OnThumbnailReady(slot->id);
                }
            }
            else if (slot->staleDiskFile)
            {
                // Delete the unloadable cache file and cache nothing; the next Get takes the
                // full Prepare and Generate path and overwrites it.
                LOG_WARNING(u8"Thumbnails", u8"stale cache file replaced for {}", slot->id);
                (void)FileDelete(slot->diskPath.AsView());
            }
            else
            {
                // Warn on generate failure: the negative cache would otherwise hide it.
                LOG_WARNING(u8"Thumbnails", u8"generate failed for {} (cached negative)",
                            slot->id);
                m_entries.InsertOrAssign(slot->id, Entry{}); // negative: stop rescheduling
            }
        }
        editor::EditorRootAllocator().Delete(slot);
    }

    void ThumbnailService::AcceptSceneResult(const Guid& id, image::Image pixels, bool ok)
    {
        // Main thread (the stage runs on the frame loop). Only the active job may complete; a
        // Reset between Take and Accept clears the flag, and the result is dropped.
        if (!m_sceneJobActive || m_activeSceneJob != id)
        {
            return;
        }
        m_sceneJobActive = false;
        m_activeSceneJob = Guid{};

        if (!ok)
        {
            LOG_WARNING(u8"Thumbnails", u8"stage render failed for {} (cached negative)", id);
            m_entries.InsertOrAssign(id, Entry{}); // negative: stop rescheduling
            return;
        }

        // Persist on the light lane (PNG encoding off the main thread) and publish immediately:
        // the drawable owns its pixels, so the UI can swap it in before the file lands.
        const u64 hash = m_contentHash ? m_contentHash(id) : 0;
        if (hash != 0 && m_jobs != nullptr)
        {
            auto* file = editor::EditorRootAllocator().New<image::Image>(pixels); // copy: publish keeps the original
            String path = CachePathFor(id, hash);
            m_jobs->SubmitLight(
                Function<void()>{[file, path]()
                                 {
                                     (void)image::io::SaveImage(*file, path.AsView(),
                                                                image::io::ImageFileFormat::PNG);
                                 }},
                Function<void()>{[file]() { editor::EditorRootAllocator().Delete(file); }});
        }

        Entry entry;
        entry.drawable = MakeRef<OwnedThumbnailDrawable>(editor::EditorRootAllocator(), Move(pixels));
        m_entries.InsertOrAssign(id, Move(entry));
        if (OnThumbnailReady)
        {
            OnThumbnailReady(id);
        }
    }
}
