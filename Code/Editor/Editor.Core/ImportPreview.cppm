// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Core - :import_preview partition
//
// What the import window shows of a file before it is imported (Specs/import-window.md). A domain
// registers a preview factory for the asset types it imports, as it registers pages; the window
// asks for the one nearest the type the file's importer lists it by, and shows its view with its
// facts under it. A type with no preview shows its facts alone. A preview reads the source file
// itself (nothing has been imported yet), off the main thread as a task on the editor's TaskPool,
// so a drop of large files never holds the window: it shows that it is reading, then its result.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <type_traits>

export module editor.core:import_preview;

import foundation.core;
import foundation.gui;
import pipeline.importer; // ImportOptions (the settings a preview may show)

using namespace foundation::core;

export namespace editor
{
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

        /// Called by the window when the file's settings changed, for a preview that shows them.
        virtual void OnSettingsChanged() {}

        /// Called by the window before it imports: stop what reads the file's prepared payload and
        /// wait for it, since the import may change that payload (a model's scale is applied to the
        /// model the importer read). A preview that reads none has nothing to stop.
        virtual void StopReading() {}

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

    /// The file a preview is made for: its path, its settings (null when its importer has none),
    /// and what its importer read of it on a worker (null when it reads nothing ahead, or has not
    /// yet), which a preview may show rather than read the file again. Both stay alive as long as
    /// the preview.
    struct ImportPreviewSource
    {
        StringView path;
        const pipeline::ImportOptions* options = nullptr;
        Object* prepared = nullptr;
    };

    /// Makes previews of the files an importer lists by one asset type (and, through nearest-type
    /// dispatch, its subclasses unless a nearer factory is registered).
    class IImportPreviewFactory
    {
    public:
        virtual ~IImportPreviewFactory() = default;

        [[nodiscard]] virtual const TypeInfo* AssetType() const = 0;
        /// A preview of `file`, any read it needs submitted to `tasks` (cancelled when the preview
        /// goes, so its completion never runs after); null when it shows nothing for it.
        [[nodiscard]] virtual UniquePtr<IImportPreview> Create(const ImportPreviewSource& file, TaskPool& tasks, IAllocator& allocator) = 0;
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
