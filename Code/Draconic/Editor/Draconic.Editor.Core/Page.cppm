// Draconic::EditorCore - :page partition.
//
// The document model (docs/design/editor.md §3.4): each open asset is an EditorPage - a dock
// tab with its OWN command stack (Sedulous/Traktor per-page undo), dirty tracking, and Save.
// This is the HEADLESS half: concrete pages live in UI-side modules (draconic.editor.app /
// draconic.<sys>.editor) and add their widget tree on top.
//
// Pages are created by IEditorPageFactory, dispatched by the instance's primary-object type
// with nearest-type matching along the base chain (Traktor's type_difference contest), via
// EditorPageRegistry.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.core:page;

import draconic.foundation;
import draconic.content;
import :command;

using namespace draconic::foundation;

export namespace draconic::editor
{
    class EditorContext; // defined in :context (pages receive it on creation)

    // One open document. Owns its command stack; the context routes Edit>Undo/Redo to the
    // active page's stack. `Commands().OnChanged` is wired by the base to mark the page dirty.
    class EditorPage
    {
    public:
        EditorPage()
        {
            m_commands.OnChanged = [this]() { m_dirty = true; };
        }
        virtual ~EditorPage() = default;
        EditorPage(const EditorPage&) = delete;
        EditorPage& operator=(const EditorPage&) = delete;

        /// Tab title (typically the instance name).
        [[nodiscard]] virtual StringView Title() const = 0;

        /// Persist the edited object(s) back to the source database. Clears dirty on success.
        [[nodiscard]] virtual Status Save() = 0;

        [[nodiscard]] bool IsDirty() const noexcept { return m_dirty; }
        void MarkDirty() noexcept { m_dirty = true; }
        void ClearDirty() noexcept { m_dirty = false; }

        [[nodiscard]] EditorCommandStack& Commands() noexcept { return m_commands; }

        /// The asset this page edits changed OUTSIDE the page (apply-to-prefab, re-import).
        /// Default no-op; pages that cache loaded content override to refresh themselves.
        virtual void OnAssetExternallyModified() {}

        /// Rebind this page to a DIFFERENT source instance (Save As): the caller created
        /// `instance` and invokes Save() next, so the page's current content lands there.
        /// Pages that cache the asset's name override (calling the base) to refresh it.
        virtual void OnSavedAs(draconic::content::Instance& instance)
        {
            m_instanceId = instance.Id();
        }

        /// The source-DB instance this page edits (zero Guid for instance-less pages).
        [[nodiscard]] const Guid& InstanceId() const noexcept { return m_instanceId; }
        void SetInstanceId(const Guid& id) noexcept { m_instanceId = id; }

    protected:
        EditorCommandStack m_commands;
        Guid m_instanceId{};
        bool m_dirty = false;
    };

    // Creates pages for one primary-object type (and, via nearest-type dispatch, its
    // subclasses unless a more specific factory is registered).
    class IEditorPageFactory
    {
    public:
        virtual ~IEditorPageFactory() = default;

        /// The primary-object type this factory's pages edit.
        [[nodiscard]] virtual const TypeInfo* PrimaryType() const = 0;

        /// Create a page editing `instance`. Null on failure (unreadable object etc.).
        [[nodiscard]] virtual UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) = 0;
    };

    // Factory registry with Traktor-style nearest-type dispatch: the factory whose
    // PrimaryType() is closest along the asset type's base chain wins.
    class EditorPageRegistry
    {
    public:
        EditorPageRegistry() = default;
        EditorPageRegistry(const EditorPageRegistry&) = delete;
        EditorPageRegistry& operator=(const EditorPageRegistry&) = delete;

        void Register(UniquePtr<IEditorPageFactory> factory)
        {
            if (factory && factory->PrimaryType() != nullptr)
            {
                m_factories.PushBack(Move(factory));
            }
        }

        [[nodiscard]] IEditorPageFactory* FindFactory(const TypeInfo& type) const
        {
            IEditorPageFactory* best = nullptr;
            u32 bestDistance = 0;
            for (const UniquePtr<IEditorPageFactory>& factory : m_factories)
            {
                u32 distance = 0;
                for (const TypeInfo* t = &type; t != nullptr; t = t->base, ++distance)
                {
                    if (t == factory->PrimaryType())
                    {
                        if (best == nullptr || distance < bestDistance)
                        {
                            best = factory.Get();
                            bestDistance = distance;
                        }
                        break;
                    }
                }
            }
            return best;
        }

        [[nodiscard]] usize Size() const noexcept { return m_factories.Size(); }

    private:
        Array<UniquePtr<IEditorPageFactory>> m_factories;
    };
}
