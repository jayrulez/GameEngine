// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Core - :import_preview partition
//
// What the import window shows of a file before it is imported (Specs/import-window.md). A domain
// registers a preview factory for the asset types it imports, as it registers pages; the window
// asks for the one nearest the type the file's importer lists it by, and shows its view with its
// facts under it. A type with no preview shows its facts alone. A preview reads the source file
// itself: nothing has been imported yet.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.core:import_preview;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor
{
    /// One file's preview, made for the window and dropped when another file is selected.
    class IImportPreview
    {
    public:
        virtual ~IImportPreview() = default;

        /// What the preview pane shows.
        [[nodiscard]] virtual foundation::gui::Node& View() = 0;
        /// The line under it, a fact each ("2048 x 2048", "RGBA, 8-bit"); empty when the file did
        /// not read.
        [[nodiscard]] virtual Span<const String> Facts() const = 0;
    };

    /// Makes previews of the files an importer lists by one asset type (and, through nearest-type
    /// dispatch, its subclasses unless a nearer factory is registered).
    class IImportPreviewFactory
    {
    public:
        virtual ~IImportPreviewFactory() = default;

        [[nodiscard]] virtual const TypeInfo* AssetType() const = 0;
        /// A preview of the source file at `path`; null when it shows nothing for it.
        [[nodiscard]] virtual UniquePtr<IImportPreview> Create(StringView path, IAllocator& allocator) = 0;
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
