// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Core - :import_preview partition
//
// What the import window shows of a file before it is imported (Specs/import-window.md). A domain
// registers a preview factory for the asset types it imports, as it registers pages; the window
// asks for the one nearest the type the file's importer lists it by, and shows its view with its
// facts under it. A type with no preview shows its facts alone. A preview reads the source file
// itself (nothing has been imported yet), off the main thread through the ImportPreviewReader, so
// a drop of large files never holds the window: it shows that it is reading, then its result.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <type_traits>

export module editor.core:import_preview;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor
{
    /// One read a preview started. Cancelled when the preview goes, so its result is dropped.
    class ImportPreviewRead : public RefCounted
    {
    public:
        void Cancel() noexcept { m_cancelled.store(true, MemoryOrder::Release); }
        [[nodiscard]] bool Cancelled() const noexcept { return m_cancelled.load(MemoryOrder::Acquire); }

    private:
        Atomic<bool> m_cancelled{false};
    };

    /// Runs the previews' reads on a worker of their own and hands each result back on the main
    /// thread. Not the editor's job service: that runs one job at a time so a cook never races the
    /// databases, and a preview queued behind a cook would wait for it; a preview only reads its
    /// source file. The worker starts with the first read.
    class ImportPreviewReader
    {
    public:
        explicit ImportPreviewReader(IAllocator& allocator) : m_allocator(&allocator), m_done(allocator) {}
        ImportPreviewReader(const ImportPreviewReader&) = delete;
        ImportPreviewReader& operator=(const ImportPreviewReader&) = delete;
        ~ImportPreviewReader()
        {
            // The reads still running finish (nothing they hold leaks); none is handed back.
            if (m_jobs)
                m_jobs->WaitForAll();
            m_jobs.Reset();
        }

        /// Runs `read` on the worker, then `done` on the main thread in Pump; each is skipped once
        /// the returned read is cancelled. `read` must not touch the GUI or anything the main
        /// thread changes: it fills what `done` shows.
        RefPtr<ImportPreviewRead> Read(Function<void()> read, Function<void()> done)
        {
            if (!m_jobs)
                m_jobs = MakeUnique<JobSystem>(*m_allocator, *m_allocator, 1u);
            RefPtr<ImportPreviewRead> ticket = MakeRef<ImportPreviewRead>(*m_allocator);
            m_inFlight.fetch_add(1, MemoryOrder::Relaxed);
            ImportPreviewReader* self = this;
            m_jobs->Submit([self, ticket, read = Move(read), done = Move(done)]() mutable
                           {
                               if (!ticket->Cancelled() && read)
                                   read();
                               read = {}; // what it captured goes here, on the worker
                               ScopedLock lock(self->m_lock);
                               self->m_done.PushBack(Finished{Move(ticket), Move(done)});
                           });
            return ticket;
        }

        /// Hands back the reads finished since the last Pump (the editor's frame calls it).
        void Pump()
        {
            Array<Finished> finished(*m_allocator);
            {
                ScopedLock lock(m_lock);
                if (m_done.IsEmpty())
                    return;
                finished = Move(m_done);
                m_done = Array<Finished>(*m_allocator);
            }
            for (Finished& read : finished)
            {
                m_inFlight.fetch_sub(1, MemoryOrder::Relaxed);
                if (!read.ticket->Cancelled() && read.done)
                    read.done();
            }
        }

        /// Reads started and not yet handed back.
        [[nodiscard]] u32 InFlight() const noexcept { return m_inFlight.load(MemoryOrder::Relaxed); }

        /// Waits for every read, then hands them back (tests; a host closing).
        void Finish()
        {
            if (m_jobs)
                m_jobs->WaitForAll();
            Pump();
        }

    private:
        struct Finished
        {
            RefPtr<ImportPreviewRead> ticket;
            Function<void()> done;
        };

        IAllocator* m_allocator;
        UniquePtr<JobSystem> m_jobs;
        Mutex m_lock; // guards m_done, which the worker fills
        Array<Finished> m_done;
        Atomic<u32> m_inFlight{0};
    };

    /// An interface a preview publishes beyond IImportPreview, which the window asks for by type
    /// (the editor's frame hooks, for a preview that renders), as a page publishes its services.
    class IImportPreviewService
    {
    protected:
        ~IImportPreviewService() = default;
    };

    /// One file's preview, made for the window and dropped when another file is selected.
    class IImportPreview
    {
    public:
        virtual ~IImportPreview() = default;

        /// The interface this preview publishes as T, or null when it publishes none.
        template <typename T>
            requires std::is_base_of_v<IImportPreviewService, T>
        [[nodiscard]] T* Service() const noexcept
        {
            IImportPreviewService* const* found = m_services.Find(TypeOf<T>().id);
            return found != nullptr ? static_cast<T*>(*found) : nullptr;
        }

        /// Set by the window: called when the preview's facts change (its read arrived).
        Function<void()> onChanged;

        /// What the preview pane shows.
        [[nodiscard]] virtual foundation::gui::Node& View() = 0;
        /// The line under it, a fact each ("2048 x 2048", "RGBA, 8-bit"); empty while it reads and
        /// when the file did not read.
        [[nodiscard]] virtual Span<const String> Facts() const = 0;

    protected:
        /// Publishes an interface this preview implements, from its constructor.
        template <typename T>
            requires std::is_base_of_v<IImportPreviewService, T>
        void Provide(T& service)
        {
            m_services.InsertOrAssign(TypeOf<T>().id, static_cast<IImportPreviewService*>(&service));
        }

    private:
        HashMap<TypeId, IImportPreviewService*> m_services;
    };

    /// Makes previews of the files an importer lists by one asset type (and, through nearest-type
    /// dispatch, its subclasses unless a nearer factory is registered).
    class IImportPreviewFactory
    {
    public:
        virtual ~IImportPreviewFactory() = default;

        [[nodiscard]] virtual const TypeInfo* AssetType() const = 0;
        /// A preview of the source file at `path`, its read started on `reader`; null when it
        /// shows nothing for it.
        [[nodiscard]] virtual UniquePtr<IImportPreview> Create(StringView path, ImportPreviewReader& reader, IAllocator& allocator) = 0;
    };

    class ImportPreviewRegistry
    {
    public:
        ImportPreviewRegistry() = default;
        ImportPreviewRegistry(const ImportPreviewRegistry&) = delete;
        ImportPreviewRegistry& operator=(const ImportPreviewRegistry&) = delete;

        void Register(UniquePtr<IImportPreviewFactory> factory)
        {
            if (factory && factory->AssetType() != nullptr)
                m_factories.PushBack(Move(factory));
        }

        /// The factory for `type`'s nearest registered type along its base chain; null when none.
        [[nodiscard]] IImportPreviewFactory* Find(const TypeInfo& type) const
        {
            for (const TypeInfo* t = &type; t != nullptr; t = t->base)
                for (const UniquePtr<IImportPreviewFactory>& factory : m_factories)
                    if (factory->AssetType() == t)
                        return factory.Get();
            return nullptr;
        }
        /// Find for the type named `typeName` (as an importer names the type it lists a file by).
        [[nodiscard]] IImportPreviewFactory* FindByName(StringView typeName) const
        {
            for (const TypeInfo* type : GlobalTypeRegistry().All())
                if (type != nullptr && type->name != nullptr && StringView(reinterpret_cast<const utf8char*>(type->name)) == typeName)
                    return Find(*type);
            return nullptr;
        }

        [[nodiscard]] usize Size() const noexcept { return m_factories.Size(); }

    private:
        Array<UniquePtr<IImportPreviewFactory>> m_factories;
    };
}
